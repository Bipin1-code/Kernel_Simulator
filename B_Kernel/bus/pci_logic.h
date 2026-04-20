
#ifndef pci_logic_h
#define pci_logic_h

#ifdef __cplusplus
extern "C" {
#endif
    
#include "pci_sim.h"  
#include <stdint.h>

    typedef struct _PCI_BAR_INFO PCI_BAR_INFO;
    
    typedef void (*fPciDeviceCallback)(uint8_t bus, uint8_t dev, uint8_t func);

    typedef void (*fPciBarChangeCallback)(uint8_t bus, uint8_t dev, uint8_t func,
                                          int bar_index, uint64_t new_address);

    void PciRegisterBarChangeCallback(fPciBarChangeCallback callback);
    
    uint16_t PciReadVendor(uint8_t bus, uint8_t dev, uint8_t func);

    uint32_t PciConfigRead32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset);

    uint32_t PciConfigWrite32(uint8_t bus, uint8_t dev, uint8_t func,
                              uint8_t offset, uint32_t value);

    void PciSetBarAddress(uint8_t bus, uint8_t dev, uint8_t func,
                          int bar_index, uint64_t address);
    
    PCI_BAR_INFO GetPciBarInfo(uint8_t bus, uint8_t dev, uint8_t func, uint32_t bar);

    void* PciGetBarMemory(uint8_t bus, uint8_t dev, uint8_t func,
                         int bar_index);
    
    uint64_t PciReadBar(uint8_t bus, uint8_t dev, uint8_t func,
                    int bar_index, uint64_t offset, int size);

    void PciWriteBar(uint8_t bus, uint8_t dev, uint8_t func,
                     int bar_index, uint64_t offset, uint64_t value, int size);
    
    void PciSetDeviceCallback(fPciDeviceCallback fpdc);

    int PciValidateAccess(uint8_t bus, uint8_t dev, uint8_t func,
                          uint64_t address, int is_io);

    int PciBridgeValidateForward(uint8_t bus, uint8_t dev, uint8_t func,
                                uint64_t address, int is_io);
    
    void PciEnumerate();

#ifdef _cplusplus
}
#endif

#endif
