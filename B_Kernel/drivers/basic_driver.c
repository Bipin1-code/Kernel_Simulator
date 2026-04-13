//This is the simple driver framework

#include "basic_driver.h"
#include "log.h"
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>

#define MAX_DRIVERS 32

//Driver registration info and access
PCI_DRIVER *g_drivers[MAX_DRIVERS];
int g_driver_count = 0;

uint16_t DummyProbe(PCI_DEVICE_CONTEXT *device){
    return (device->vendor_id == 0xB350);
}

int DummyInit(PCI_DEVICE_CONTEXT *device){
    (void)device;
    LOG_INFO("Dummy Driver initialized!");
    return 0;
}

static void DriverManagerHandleNewDevice(PCI_DEVICE_CONTEXT *device){
    LOG_INFO("DEBUG: Driver Manager received new device");

    for(int i = 0; i < g_driver_count; i++){
        PCI_DRIVER *drv = g_drivers[i];
        if(!drv || !drv->probe) continue;

        if(drv->probe(device)){
            device->driver = drv;
            LOG_INFO_FMT("DRIVER %s attached to device %04x:%04x",
                         drv->name, device->vendor_id, device->device_id);
            if(drv->init){
                drv->init(device);
            }
            return;
        }
    }
    LOG_INFO("No driver found for device");
}

void DriverFrameworkInit(){
    LOG_INFO("Driver initialized!");
    OS_RegisterDeviceFoundCallback(DriverManagerHandleNewDevice);
    return;
}

//Driver registration station
void RegisterDriver(PCI_DRIVER *drv){
    if(g_driver_count >= MAX_DRIVERS){
        LOG_ERROR_FMT("Max Driver capcity %d, OVERFLOWED",
                      MAX_DRIVERS);
        return;
    }
    g_drivers[g_driver_count++] = drv;
}

static int TestBarDriverInit(PCI_DEVICE_CONTEXT *device){
    LOG_INFO("BAR Test Driver initializing...");
    
    // Find first memory BAR
    int mem_bar = -1;
    for(int i = 0; i < 6; i++){
        if(device->bar_info[i].size > 0 && !device->bar_info[i].is_io){
            mem_bar = i;
            break;
        }
    }
    
    if(mem_bar == -1){
        LOG_ERROR("No memory BAR found!");
        return -1;
    }
    
    LOG_INFO_FMT("Testing BAR%d - Size: 0x%"
                 PRIx64, mem_bar, device->bar_info[mem_bar].size);
    
    // Test 1: Direct memory access
    uint8_t *mem = (uint8_t*)device->fBarGetMem(device, mem_bar);
    if(mem){
        // Write test pattern
        for(int i = 0; i < 16; i++){
            mem[i] = 0xA0 + i;
        }
        
        // Verify
        int ok = 1;
        for(int i = 0; i < 16; i++){
            if(mem[i] != 0xA0 + i){
                ok = 0;
                LOG_ERROR_FMT("Mismatch at %d: expected 0x%02X, got 0x%02X",
                              i, 0xA0 + i, mem[i]);
            }
        }
        LOG_INFO_FMT("%s Direct memory test", ok ? "Yes" : "No");
    }
    
    // Test 2: Read/Write interface
    device->fBarWrite(device, mem_bar, 0x100, 0xDFBADEEF, 4);
    uint32_t val = (uint32_t)device->fBarRead(device, mem_bar, 0x100, 4);
    LOG_INFO_FMT("%s Read/Write test (0x%08X)", 
                 val ==  0xDFBADEEF ? "Yes" : "No", val);
    
    return 0;
}

static uint16_t TestBarDriverProbe(PCI_DEVICE_CONTEXT *device){
    // Probe any device with a decent sized memory BAR
    for(int i = 0; i < 6; i++){
        if(device->bar_info[i].size >= 0x1000 && !device->bar_info[i].is_io){
            return 50;  //Bingo!!
        }
    }
    return 0;
}

void DriverPool(){
    /* static PCI_DRIVER dummy_driver = (PCI_DRIVER){ */
    /*     .name = "Dummy Driver", */
    /*     .vendor_id = 0xB350, */
    /*     .device_id = 0x0000, */
    /*     .probe = DummyProbe, */
    /*     .init = DummyInit, */
    /* }; */
    /* RegisterDriver(&dummy_driver); */
    
    static PCI_DRIVER driver = {
        .name = "BAR Test Driver",
        .vendor_id = 0xB350,
        .device_id = 0x1B10,
        .probe = TestBarDriverProbe,
        .init = TestBarDriverInit,
    };
    RegisterDriver(&driver);
}
