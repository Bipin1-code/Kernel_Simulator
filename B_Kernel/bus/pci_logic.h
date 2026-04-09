
#pragma once

#include "pci_sim.h"
#include <stdint.h>

typedef struct _PCI_BAR_INFO PCI_BAR_INFO;

uint16_t PciReadVendor(uint8_t bus, uint8_t dev, uint8_t func);

uint32_t PciConfigRead32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);

uint32_t PciConfigWrite32(uint8_t bus, uint8_t dev, uint8_t func,
                          uint8_t offset, uint32_t value);

PCI_BAR_INFO GetPciBarInfo(uint8_t bus, uint8_t dev, uint8_t func, uint32_t bar);

void OnPciDeviceFound(uint8_t bus, uint8_t dev, uint8_t func);

void PciEnumerate();
