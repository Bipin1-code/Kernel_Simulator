
#ifndef OS_device_h
#define OS_device_h

#ifdef __cplusplus
extern "C" {
#endif

#include "pci_logic.h"
#include <stdint.h>
#define MAX_REAL_DEVICE 8192

    typedef struct _PCI_BAR_INFO{
        uint64_t base_address;
        uint64_t size;
        uint8_t is_io;
        uint8_t is_64bit;
    } PCI_BAR_INFO;

    typedef struct _PCI_DEVICE_CONTEXT{
        uint8_t bus;
        uint8_t dev;
        uint8_t func;
        uint16_t vendor_id;
        uint16_t device_id;
        uint8_t revision_id;
        uint8_t prog_if;
        uint8_t subclass;
        uint8_t class_code;
        uint8_t header_type;

        union{
            uint32_t type0_bars[6];
            uint32_t type1_bars[2];
        } select_bar;
    
        PCI_BAR_INFO bar_info[6];    

        //new field added
        void *driver;
        void *device_data;

        void* (*fBarGetMem)(struct _PCI_DEVICE_CONTEXT *ctx, int bar_index);
        uint64_t (*fBarRead)(struct _PCI_DEVICE_CONTEXT *ctx, int bar, uint64_t offset,
                             int size);
        void (*fBarWrite)(struct _PCI_DEVICE_CONTEXT *ctx, int bar, uint64_t offset,
                          uint64_t value, int size);
    
    } PCI_DEVICE_CONTEXT;

    extern PCI_DEVICE_CONTEXT *g_pciDevCtx[MAX_REAL_DEVICE];
    extern int g_dev_count;
    
    PCI_DEVICE_CONTEXT* DeviceCreateFromPci(uint8_t bus, uint8_t dev, uint8_t func);
    void OS_HandleNewPciDevice(uint8_t bus, uint8_t dev, uint8_t func);
    void OS_RegisterDeviceFoundCallback(void (*callback)(PCI_DEVICE_CONTEXT *));

#ifdef __cplusplus
}
#endif

#endif
