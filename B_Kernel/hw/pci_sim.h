
#pragma once

#include <stdint.h>

#define MAX_BUS 2
#define MAX_DEVICE 32
#define MAX_FUNCTION 8

typedef struct _PCI_DEVICE PCI_DEVICE;

extern PCI_DEVICE *g_pci_bus[MAX_BUS][MAX_DEVICE][MAX_FUNCTION];

void PciDevicePool();
