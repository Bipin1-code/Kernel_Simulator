
#ifndef PCI_SIM_INTERNAL_H
#define PCI_SIM_INTERNAL_H

#include <stdint.h>

uint8_t* PciSimGetConfig(uint8_t bus, uint8_t dev, uint8_t func);

uint64_t PciSimGetBarSize(uint8_t bus, uint8_t dev, uint8_t func,
                          int bar_index);

void* PciSimGetBarMemory(uint8_t bus, uint8_t dev, uint8_t func,
                         int bar_index);
uint64_t PciSimReadBar(uint8_t bus, uint8_t dev, uint8_t func,
                       int bar_index, uint64_t offset, int size);

void PciSimWriteBar(uint8_t bus, uint8_t dev, uint8_t func,
                    int bar_index, uint64_t offset, uint64_t value,
                    int size);

#endif
