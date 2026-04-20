
#ifndef PCI_SIM_INTERNAL_H
#define PCI_SIM_INTERNAL_H

#include <stdint.h>

//Command Register Bit define (0x04-0x05)
#define PCI_CMD_IO_SPACE      0x0001
#define PCI_CMD_MEMORY_SPACE  0x0002   
#define PCI_CMD_BUS_MASTER    0x0004    
#define PCI_CMD_SPECIAL_CYCLE 0x0008   
#define PCI_CMD_MWI_ENABLE    0x0010
#define PCI_CMD_VGA_SNOOP     0x0020
#define PCI_CMD_PARITY_ERROR  0x0040
#define PCI_CMD_STEPPING      0x0080
#define PCI_CMD_SERR_ENABLE   0x0100
#define PCI_CMD_FAST_B2B      0x0200
#define PCI_CMD_INT_DISABLE   0x0400
//bit 11-15: reserved (read = 0, ignore writes)

#define PCI_CMD_SUPPORTED_BASIC (PCI_CMD_IO_SPACE | PCI_CMD_MEMORY_SPACE | \
                                 PCI_CMD_BUS_MASTER)

#define PCI_CMD_SUPPORTED_FULL (PCI_CMD_SUPPORTED_BASIC | PCI_CMD_MWI_ENABLE | \
                                PCI_CMD_PARITY_ERROR | PCI_CMD_SERR_ENABLE)

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

//command register helper
uint16_t PciSimGetDeviceCommandMask(PCI_DEVICE *dev);

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
