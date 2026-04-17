
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


// Discovery Mode Accessor functions
void PciSimSetDiscoveryMode(uint8_t bus, uint8_t dev, uint8_t func, int active);

int PciSimGetDiscoveryMode(uint8_t bus, uint8_t dev, uint8_t func);

void PciSimSetDiscoveryMaskLow(uint8_t bus, uint8_t dev, uint8_t func,
                               int bar_index, uint32_t mask);

void PciSimSetDiscoveryMaskHigh(uint8_t bus, uint8_t dev, uint8_t func,
                               int bar_index, uint32_t mask);

uint32_t PciSimGetDiscoveryMaskLow(uint8_t bus, uint8_t dev, uint8_t func,
                                   int bar_index);

uint32_t PciSimGetDiscoveryMaskHigh(uint8_t bus, uint8_t dev, uint8_t func,
                                   int bar_index);

void PciSimClearDiscovery(uint8_t bus, uint8_t dev, uint8_t func);

#endif
