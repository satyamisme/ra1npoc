#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/usb/IOUSBLib.h>
#include <IOKit/IOCFPlugIn.h>

#include <iousb.h>


extern io_client_t client;

static const char *darwin_device_class = kIOUSBDeviceClassName;

static int nsleep(long nanoseconds) {
    struct timespec req, rem;
    req.tv_sec = 0;
    req.tv_nsec = nanoseconds;
    return nanosleep(&req, &rem);
}

static int check_context(io_client_t client) {
    if (client == NULL || client->handle == NULL) {
        return -1;
    }
    return 0;
}

const struct io_devinfo* io_get_device_info(io_client_t client)
{
    if (check_context(client) != 0)
        return NULL;
    return &client->devinfo;
}

static void iokit_cfdictionary_set_short(CFMutableDictionaryRef dict, const void *key, SInt16 value)
{
    CFNumberRef numberRef;
    numberRef = CFNumberCreate(kCFAllocatorDefault, kCFNumberSInt16Type, &value);
    if (numberRef) {
        CFDictionarySetValue(dict, key, numberRef);
        CFRelease(numberRef);
    }
}

static io_iterator_t io_get_iterator_for_pid(uint16_t pid) {
    
    IOReturn result;
    io_iterator_t iterator;
    CFMutableDictionaryRef matchingDict;
    
#ifdef IPHONEOS_ARM
    // Allows iOS to connect to iOS devices.
    darwin_device_class = "IOUSBHostDevice";
#endif
    
    matchingDict = IOServiceMatching(darwin_device_class);
    iokit_cfdictionary_set_short(matchingDict, CFSTR(kUSBVendorID), kAppleVendorID);
    iokit_cfdictionary_set_short(matchingDict, CFSTR(kUSBProductID), pid);
    
    result = IOServiceGetMatchingServices(kIOMasterPortDefault, matchingDict, &iterator);
    if (result != kIOReturnSuccess){
        return IO_OBJECT_NULL;
    }
    
    return iterator;
}

static void load_devinfo(io_client_t client, const char* str)
{
    if (!client || !str) {
        return;
    }
    
    char* ptr;
    char tmp[256];
    
    memset(&client->devinfo, '\0', sizeof(struct io_devinfo));
    memset(&tmp, '\0', 256);
    
    ptr = strstr(str, "SDOM:");
    if (ptr != NULL) {
        sscanf(ptr, "SDOM:%x", &client->devinfo.sdom);
    }
    
    ptr = strstr(str, "CPID:");
    if (ptr != NULL) {
        sscanf(ptr, "CPID:%x", &client->devinfo.cpid);
    }
    
    ptr = strstr(str, "BDID:");
    if (ptr != NULL) {
        sscanf(ptr, "BDID:%x", &client->devinfo.bdid);
    }
    
    ptr = strstr(str, "SRNM:[");
    if(ptr != NULL) {
        client->devinfo.hasSRNM = TRUE;
    } else {
        client->devinfo.hasSRNM = FALSE;
    }
    
    ptr = strstr(str, "PWND:[");
    if(ptr != NULL) {
        client->devinfo.hasPWND = TRUE;
    } else {
        client->devinfo.hasPWND = FALSE;
    }
    
    tmp[0] = '\0';
    ptr = strstr(str, "SRTG:[");
    if(ptr != NULL) {
        sscanf(ptr, "SRTG:[%s]", tmp);
        ptr = strrchr(tmp, ']');
        if(ptr != NULL) {
            *ptr = '\0';
        }
        client->devinfo.srtg = strdup(tmp);
    }
}

static int io_usb_set_config(io_client_t client, int conf) {
    
    if (check_context(client) != 0)
        return -1;
    
    IOReturn result;
    result = (*client->handle)->SetConfiguration(client->handle, conf);
    if (result != kIOReturnSuccess) {
        return -1;
    }
    
    return 0;
}

void io_close(io_client_t client){
    if (client->handle) {
        (*client->handle)->USBDeviceClose(client->handle);
        (*client->handle)->Release(client->handle);
        client->handle = NULL;
    }
    if(client->async_event_source) {
        CFRunLoopRemoveSource(CFRunLoopGetCurrent(), client->async_event_source, kCFRunLoopDefaultMode);
        CFRelease(client->async_event_source);
    }
    free(client);
    client = NULL;
}

// for iOS 10 or lower
void SNR(io_client_t client){
    IOReturn r;
    uint8_t size;
    unsigned char buf[0x100];
    unsigned char str[0x100];
    bzero(str, 0x100);
    bzero(buf, 0x100);
    
    if(client->devinfo.srtg == NULL){
        r = usb_ctrl_transfer(client, 0x80, 6, 0x0306, 0x040a, buf, 0x100); // 8950 or up (PWND)
        if(r != kIOReturnSuccess) return;
        size = *(uint8_t*)buf;
        for(int i=0;i<(size/2);i++){
            str[i] = *(uint8_t*)(buf+2+(i*2));
        }
        load_devinfo(client, (const char*)str);
    }
    
    if(client->devinfo.srtg == NULL){
        r = usb_ctrl_transfer(client, 0x80, 6, 0x0304, 0x040a, buf, 0x100); // 8950 or up
        if(r != kIOReturnSuccess) return;
        size = *(uint8_t*)buf;
        for(int i=0;i<(size/2);i++){
            str[i] = *(uint8_t*)(buf+2+(i*2));
        }
        load_devinfo(client, (const char*)str);
    }
    
    if(client->devinfo.srtg == NULL){
        bzero(buf, 0x100);
        bzero(str, 0x80);
        r = usb_ctrl_transfer(client, 0x80, 6, 0x0303, 0x040a, buf, 0x100); // 8930
        if(r != kIOReturnSuccess) return;
        size = *(uint8_t*)buf;
        for(int i=0;i<(size/2);i++){
            str[i] = *(uint8_t*)(buf+2+(i*2));
        }
        load_devinfo(client, (const char*)str);
    }
    
    if(client->devinfo.srtg != NULL){
        client->hasSerialStr = TRUE;
    }
    
}

static int io_usb_open_service(io_client_t *pclient, io_service_t service){
    int r;
    IOReturn result;
    io_client_t client;
    IOCFPlugInInterface **plug = NULL;
    SInt32 score;
    UInt16 mode;
    UInt32 locationID;
    CFStringRef serialString;
    
    client = (io_client_t) calloc( 1, sizeof(struct io_client_p));
    
#ifndef IPHONEOS_LOWSPEC
    // Create the plug-in
    const int max_retries = 5;
    for (int count = 0; count < max_retries; count++) {
        result = IOCreatePlugInInterfaceForService(service, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score);
        if (kIOReturnSuccess == result && plug) {
            break;
        }
        nanosleep(&(struct timespec){.tv_sec = 0, .tv_nsec = 1000}, NULL);
    }
#else
    result = IOCreatePlugInInterfaceForService(service, kIOUSBDeviceUserClientTypeID, kIOCFPlugInInterfaceID, &plug, &score);
#endif
    
    if (result != kIOReturnSuccess) {
        IOObjectRelease(service);
        free(client);
        return -1;
    }
    
    // Older iOS versions (such as iOS 10) can't get the serial number, so don't get it.
    char serial_str[256];
    serial_str[0] = '\0';
    serialString = IORegistryEntryCreateCFProperty(service, CFSTR(kUSBSerialNumberString), kCFAllocatorDefault, 0);
    if (serialString) {
        CFStringGetCString(serialString, serial_str, sizeof(serial_str), kCFStringEncodingUTF8);
        CFRelease(serialString);
        load_devinfo(client, serial_str);
        client->hasSerialStr = TRUE;
    } else {
        client->hasSerialStr = FALSE;
    }
    
    IOObjectRelease(service);
    
    // Create the device interface
    result = (*plug)->QueryInterface(plug, CFUUIDGetUUIDBytes(kIOUSBDeviceInterfaceID320), (LPVOID *)&(client->handle));
    IODestroyPlugInInterface(plug);
    if (result != kIOReturnSuccess) {
        free(client);
        return -1;
    }
    
    (*client->handle)->GetDeviceProduct(client->handle, &mode);
    (*client->handle)->GetLocationID(client->handle, &locationID);
    client->mode = mode;
    
    result = (*client->handle)->USBDeviceOpen(client->handle);
    
    if (result != kIOReturnSuccess) {
        (*client->handle)->Release(client->handle);
        free(client);
        return -1;
    }

    r = io_usb_set_config(client, 1);
    if (r != 0) {
        free(client);
        return r;
    }
    
    r = (*client->handle)->CreateDeviceAsyncEventSource(client->handle, &client->async_event_source);
    if (r != 0) {
        free(client);
        return r;
    }
    
    CFRunLoopAddSource(CFRunLoopGetCurrent(), client->async_event_source, kCFRunLoopDefaultMode);
    
    *pclient = client;
    return 0;
}

static void io_async_cb(void *refcon, IOReturn ret, void *arg_0)
{
    async_transfer_t* transfer = refcon;
    
    if(transfer != NULL) {
        transfer->ret = ret;
        memcpy(&transfer->len, &arg_0, sizeof(transfer->len));
        CFRunLoopStop(CFRunLoopGetCurrent());
    }
}

int io_open(io_client_t *pclient, uint16_t pid){
    int r;
    
    io_service_t service = IO_OBJECT_NULL;
    io_iterator_t iterator;
    client = NULL;
    iterator = io_get_iterator_for_pid(pid);
    
    if (iterator != IO_OBJECT_NULL) {
        while((service = IOIteratorNext(iterator)) != IO_OBJECT_NULL) {
            break;
        }
        IOObjectRelease(iterator);
        return io_usb_open_service(pclient, service);
    }
    return -1;
}

int get_device(unsigned int mode) {
    
    if(client) {
        io_close(client);
        client = NULL;
    }
    
    io_open(&client, mode);
    if(!client) {
        return -1;
    }
    
    if(client->mode != mode){
        io_close(client);
        client = NULL;
        return -1;
    }
    
    return 0;
}

int io_reset(io_client_t client) {
    
    if (check_context(client) != 0)
        return -1;
    
    IOReturn result;
    
    result = (*client->handle)->ResetDevice(client->handle);
    if (result != kIOReturnSuccess && result != kIOReturnNotResponding) {
        return -1;
    }
    
    result = (*client->handle)->USBDeviceReEnumerate(client->handle, 0);
    if (result != kIOReturnSuccess && result != kIOReturnNotResponding) {
        // error re-enumerating device: result (ignored)
    }
    
    return 0;
}

int get_device_time_stage(io_client_t *pclient, unsigned int time, uint16_t stage){
#ifndef IPHONEOS_LOWSPEC
    time = time*4;
#endif
    for(int i=0; i<time; i++){
        if(*pclient) {
            io_close(*pclient);
            *pclient = NULL;
        }
        if (io_open(pclient, stage) != 0) {
            // Connection failed. Waiting 1 sec before retry.
#ifndef IPHONEOS_LOWSPEC
            usleep(250000);
#else
            sleep(1);
#endif
        } else {
            return 0;
        }
    }
    return -1;
}

IOReturn usb_ctrl_transfer(io_client_t client, uint8_t bm_request_type, uint8_t b_request, uint16_t w_value, uint16_t w_index, unsigned char *data, uint16_t w_length)
{
    IOReturn result;
    IOUSBDevRequest req;
    
    bzero(&req, sizeof(req));
    req.bmRequestType     = bm_request_type;
    req.bRequest          = b_request;
    req.wValue            = OSSwapLittleToHostInt16(w_value);
    req.wIndex            = OSSwapLittleToHostInt16(w_index);
    req.wLength           = OSSwapLittleToHostInt16(w_length);
    req.pData             = data;
    
    result = (*client->handle)->DeviceRequest(client->handle, &req);
    
    return result;
}

IOReturn usb_ctrl_transfer_with_time(io_client_t client, uint8_t bm_request_type, uint8_t b_request, uint16_t w_value, uint16_t w_index, unsigned char *data, uint16_t w_length, unsigned int time)
{
    IOReturn result;
    IOUSBDevRequestTO req;
    
    bzero(&req, sizeof(req));
    req.bmRequestType     = bm_request_type;
    req.bRequest          = b_request;
    req.wValue            = OSSwapLittleToHostInt16(w_value);
    req.wIndex            = OSSwapLittleToHostInt16(w_index);
    req.wLength           = OSSwapLittleToHostInt16(w_length);
    req.pData             = data;
    req.noDataTimeout     = time;
    req.completionTimeout = time;
    
    result = (*client->handle)->DeviceRequestTO(client->handle, &req);
    
    return result;
}

IOReturn io_abort_pipe_zero(io_client_t client){
    return (*client->handle)->USBDeviceAbortPipeZero(client->handle);
}

IOReturn async_usb_ctrl_transfer(io_client_t client, uint8_t bm_request_type, uint8_t b_request, uint16_t w_value, uint16_t w_index, unsigned char *data, uint16_t w_length, async_transfer_t* transfer)
{
    IOReturn result;
    IOUSBDevRequest req;
    
    bzero(&req, sizeof(req));
    req.bmRequestType     = bm_request_type;
    req.bRequest          = b_request;
    req.wValue            = OSSwapLittleToHostInt16(w_value);
    req.wIndex            = OSSwapLittleToHostInt16(w_index);
    req.wLength           = OSSwapLittleToHostInt16(w_length);
    req.pData             = data;
    result = (*client->handle)->DeviceRequestAsync(client->handle, &req, io_async_cb, transfer);
    
    return result;
}

uint32_t async_usb_ctrl_transfer_with_cancel(io_client_t client, uint8_t bm_request_type, uint8_t b_request, uint16_t w_value, uint16_t w_index, unsigned char *data, uint16_t w_length, unsigned int ns_time)
{
    IOReturn error;
    async_transfer_t transfer;
    bzero(&transfer, sizeof(async_transfer_t));
    
    error = async_usb_ctrl_transfer(client, bm_request_type, b_request, w_value, w_index, data, w_length, &transfer);
    if(error != kIOReturnSuccess) {
        return error;
    }
    nsleep(ns_time);
    
    error = io_abort_pipe_zero(client);
    if(error != kIOReturnSuccess) {
        return -1;
    }
    while(transfer.ret != kIOReturnAborted){
        CFRunLoopRun();
    }
    
    return transfer.len;
}

uint32_t async_usb_ctrl_transfer_no_error(io_client_t client, uint8_t bm_request_type, uint8_t b_request, uint16_t w_value, uint16_t w_index, unsigned char *data, uint16_t w_length)
{
    async_transfer_t transfer;
    bzero(&transfer, sizeof(async_transfer_t));
    
    async_usb_ctrl_transfer(client, bm_request_type, b_request, w_value, w_index, data, w_length, &transfer);
    return transfer.len;
}

uint32_t async_usb_ctrl_transfer_with_cancel_noloop(io_client_t client, uint8_t bm_request_type, uint8_t b_request, uint16_t w_value, uint16_t w_index, unsigned char *data, uint16_t w_length, unsigned int ns_time)
{
    async_transfer_t transfer;
    bzero(&transfer, sizeof(async_transfer_t));
    
    async_usb_ctrl_transfer(client, bm_request_type, b_request, w_value, w_index, data, w_length, &transfer);
    nsleep(ns_time);
    io_abort_pipe_zero(client);
    
    return transfer.len;
}
