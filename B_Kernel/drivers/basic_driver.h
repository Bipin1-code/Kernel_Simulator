
#ifndef basic_driver_h
#define basic_driver_h

#ifdef __cplusplus
extern "C" {
#endif
    
#include "OS_device.h"

#define MAX_DRIVERS 32
    
    //Driver Description or Definition
    typedef struct _PCI_DRIVER{
        char name[24];
        //This device driver owns
        uint16_t vendor_id;
        uint16_t device_id;

        //how driver make devices to do work 
        int (*init)(PCI_DEVICE_CONTEXT *device);
        uint16_t (*probe)(PCI_DEVICE_CONTEXT *device);
    } PCI_DRIVER;

    extern PCI_DRIVER *g_drivers[MAX_DRIVERS];
    extern int g_driver_count;

    void RegisterDriver(PCI_DRIVER *drv);
    void DriverPool();
    void DriverFrameworkInit();

#ifdef __cplusplus
}
#endif

#endif
