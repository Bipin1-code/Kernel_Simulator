
#pragma once

#include "pci_logic.h"

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
} PCI_DEVICE_CONTEXT;

PCI_DEVICE_CONTEXT* DeviceCreateFromPci(const uint8_t bus, const uint8_t dev,
                                   const  uint8_t func);
