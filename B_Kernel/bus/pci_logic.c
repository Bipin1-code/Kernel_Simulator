
#include "pci_logic.h"
#include "pci_sim.h"
#include "log.h"
#include <stdio.h>
#include <stdint.h>
#include <inttypes.h>

typedef struct _PCI_BAR_INFO{
    uint64_t base_address;
    uint64_t size;
    uint8_t is_io;
    uint8_t is_64bit;
} PCI_BAR_INFO;

uint8_t* PciSimGetConfig(uint8_t bus, uint8_t dev, uint8_t func);

uint64_t PciSimGetBarSize(uint8_t bus, uint8_t dev, uint8_t func,
                          int bar_index);

uint16_t PciReadVendor(uint8_t bus, uint8_t dev, uint8_t func){
    PCI_DEVICE *device = g_pci_bus[bus][dev][func];
    if(!device){
        /* LOG_INFO("No device found"); */
        return 0xffff;
    }
    return *(uint16_t *)(PciSimGetConfig(bus, dev, func) + 0x00);
}

uint32_t PciConfigRead32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset){
    PCI_DEVICE *device = g_pci_bus[bus][dev][func];
    if(!device) return 0xffffffff;
    return *(uint32_t *)(PciSimGetConfig(bus, dev, func) + offset);
}

uint32_t PciConfigWrite32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset,
                          uint32_t value){
    PCI_DEVICE *device = g_pci_bus[bus][dev][func];
    if(!device) return 0xffffffff;

    uint8_t *begin_data = PciSimGetConfig(bus, dev, func); 
    
    if(offset >= 0x10 && offset < 0x28){
        /* Mapping config-space offset -> BAR index */
        int bar_index = (offset - 0x10) / 4;

        //prev BARs offset as helper
        uint8_t low_offset = offset - 4;
        //flag for current BAR type (32-bit/ 64-bit)
        int is_high_dword_of_64bit = 0;
        uint64_t size = 0;
        int bar_index_low = -1;

        if((offset - 0x10) % 8 == 4 && offset > 0x10){
            //this might be a high dword - check the low dword
            uint32_t low_value = *(uint32_t *)(begin_data + low_offset);
            uint32_t type = (low_value >> 1) & 0x3;
            if(type == 2){
                is_high_dword_of_64bit = 1;
                bar_index_low = bar_index - 1;
                if(bar_index_low >= 0 && bar_index_low < 6){
                    size = PciSimGetBarSize(bus, dev, func, bar_index_low);
                }
            }
        }
        if(value == 0xffffffff && is_high_dword_of_64bit){
            //this is the high dword of a 64-bit BAT during size discovery
            if(size > 0){
                uint64_t mask64 = ~(size - 1);
                uint32_t high_mask = (uint32_t)(mask64 >> 32);
                printf("DEBUG PciConfigWrite32: Writing high dword of 64-bit BAR to 0x%08X\n",
                       high_mask);
                *(uint32_t *)(begin_data + offset) = high_mask;
                return 0;
            }
        }

        uint32_t orig = *(uint32_t *)(begin_data + offset);
        uint32_t type = (orig >> 1) & 0x3;
        int is_64 = (type == 2);
        
        if(value == 0xffffffff){
            if(bar_index >= 0 && bar_index < 6){
                size = PciSimGetBarSize(bus, dev, func, bar_index);
            }
            if(size == 0){
                *(uint32_t *)(begin_data + offset) = 0;
                if(is_64 && ((offset - 0x10) % 8) == 0 && bar_index + 1 < 6){
                    *(uint32_t *)(begin_data + offset + 4) = 0;
                }
                return 0;
            }
            if(orig & 1){
                //Type = IO BAR
                uint32_t mask = (uint32_t)(~(size - 1)) & ~0x3;
                *(uint32_t *)(begin_data + offset) = mask | (orig & 0x3);
            }else{
                //Memory BAR
                if(is_64){
                    int is_lower64Bits = ((offset - 0x10) % 8) == 0;
                    if(is_lower64Bits){
                        uint64_t mask64 = ~(size - 1);
                        uint32_t low_mask = (uint32_t)(mask64 & 0xffffffff);
                        
                        *(uint32_t *)(begin_data + offset) =
                            (low_mask & ~0xf) | (orig & 0xf);
                        
                        if(bar_index + 1 < 6){
                            uint32_t high_mask = (uint32_t)(mask64 >> 32);
                            *(uint32_t *)(begin_data + offset + 4) = high_mask;
                        }
                    }else{
                        uint64_t mask64 = ~(size - 1);
                        uint32_t high_mask = (uint32_t)(mask64 >> 32);
                        printf("DEBUG PciConfigWrite32: Writing to high dword of BAR%d, value=0x%08X\n", 
                               bar_index, high_mask);
                        *(uint32_t *)(begin_data + offset) = high_mask;
                    }
                }else{
                    uint32_t mask = (uint32_t)(~(size - 1)) & ~0xf;
                    *(uint32_t *)(begin_data + offset) =
                        mask | (orig & 0xf); 
                }
            }
            return 0;
        }
    }
    *(uint32_t *)(begin_data + offset) = value;
    return 0;
}

PCI_BAR_INFO GetPciBarInfo(uint8_t bus, uint8_t dev, uint8_t func, uint32_t bar){
    PCI_BAR_INFO pbi = {0};

    uint8_t offset = 0x10 + bar * 4;
    //original 
    uint32_t orig_low = PciConfigRead32(bus, dev, func, offset);
    
    //Finall get all info about bar and them
    if(orig_low & 1){
        //IO BAR
        PciConfigWrite32(bus, dev, func, offset, 0xffffffff);
        uint32_t size_low = PciConfigRead32(bus, dev, func, offset);

        //restore
        PciConfigWrite32(bus, dev, func, offset, orig_low);
        
        if(size_low != 0 && size_low != 0xffffffff){
            pbi.is_io = 1;
            pbi.is_64bit = 0;
            uint32_t mask = size_low & ~0x3;
            pbi.size = (~mask) + 1;
            pbi.base_address = (orig_low & ~0x3);
            printf("DEBUG: IO BAR size=0x%" PRIx64 "\n", pbi.size);
        }
    }else{
        //Memory BAR
        uint32_t type = (orig_low >> 1) & 0x3;
        uint8_t is_64 = (type == 2);
        uint32_t orig_high = 0;
        
        if(is_64){
            if(bar >= 5){  //Need bar and bar+1 to exist
                LOG_WARN_FMT("64-bit BAR at index %d exceeds available slots", bar);
                return pbi;
            }
            orig_high = PciConfigRead32(bus, dev, func, offset + 4);
        }     
        
        PciConfigWrite32(bus, dev, func, offset, 0xffffffff);
        uint32_t size_low = PciConfigRead32(bus, dev, func, offset);
        uint32_t size_high = 0;
        if(is_64){
            PciConfigWrite32(bus, dev, func, offset + 4, 0xffffffff);
            size_high = PciConfigRead32(bus, dev, func, offset + 4);
        }
        printf("DEBUG:(Response) BAR%d size_low=0x%08X size_high=0x%08X\n",
               bar, size_low, size_high);
   
        //Restore 
        PciConfigWrite32(bus, dev, func, offset, orig_low);
        if(is_64){
            PciConfigWrite32(bus, dev, func, offset + 4, orig_high);
        }
        
        if(size_low != 0 && size_low != 0xffffffff){
            pbi.is_io = 0;
            pbi.is_64bit = is_64;
            if(is_64){
                uint64_t mask = ((uint64_t)size_low & ~0xfU) | ((uint64_t)size_high << 32);
                pbi.size = (~mask) + 1;
                
                if(pbi.size == 0 && mask != 0){
                    pbi.size = (uint64_t)1 << 63;
                }
                pbi.base_address = ((uint64_t)orig_high << 32) | (orig_low & ~0xfU);
                printf("DEBUG: 64-bit BAR full_mask=0x%016" PRIX64 " size=0x%" PRIx64 " base=0x%" PRIx64 "\n", 
                       mask, pbi.size, pbi.base_address);
            }else{
                uint32_t mask = size_low & ~0xfU; 
                pbi.size = (~mask) + 1;
                pbi.base_address = (orig_low & ~0xfU);
                printf("DEBUG: 32-bit BAR size=0x%" PRIx64 " base=0x%" PRIx64 "\n", 
                       pbi.size, pbi.base_address);
            } 
        }
    }
    if((pbi.size == 0) || (pbi.size & (pbi.size - 1)) || pbi.size < 4){
        LOG_WARN_FMT("Invalid BAR size detected: 0x%" PRIx64, pbi.size);
        pbi.size = 0;
    }else{
        printf("DEBUG: Valid BAR size: 0x%" PRIx64 "\n", pbi.size);
    }
    return pbi;
}


static uint8_t PciGetHeaderType(uint8_t bus, uint8_t dev, uint8_t func){
    uint32_t val = PciConfigRead32(bus, dev, func, 0x0c);
    return (val >> 16) & 0x7f;
}

static uint8_t PciGetSecondaryBus(uint8_t bus, uint8_t dev, uint8_t func){
    uint32_t val = PciConfigRead32(bus, dev, func, 0x18);
    return (val >> 8) & 0xff;
}

//callback function
void OnPciDeviceFound(uint8_t bus, uint8_t dev, uint8_t func);

static void PciScanBus(uint8_t bus){
    for(int dev = 0; dev < MAX_DEVICE; dev++){ 
        for(int func = 0; func < MAX_FUNCTION; func++){
            if(func == 0){
                uint16_t vendor = PciReadVendor(bus, dev, 0);
                if(vendor == 0xFFFF)
                    break;
            }
            uint16_t vendor = PciReadVendor(bus, dev, func);
            if(vendor == 0xffff) continue;
            puts("\n");
            LOG_INFO("PCI DEVICE FOUND");
            LOG_INFO_FMT("Bus=%d, Dev= %d, func= %d", bus, dev, func);

            //callback hook
            OnPciDeviceFound(bus, dev, func);
            
            uint8_t header = PciGetHeaderType(bus, dev, func);
            if(header == 1){
                LOG_INFO("Bridge device detected");
                uint8_t sec_bus = PciGetSecondaryBus(bus, dev, func);
    
                if(sec_bus != bus && sec_bus < MAX_BUS){
                    LOG_INFO_FMT("Scanning Secondary Bus %d", sec_bus);
                    PciScanBus(sec_bus);
                }
            }
        }
    }
}

void PciEnumerate(){
    PciScanBus(0);
}
