
#include "pci_logic.h"
#include "pci_sim.h"
#include "pci_sim_internal.h"
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

//callback function
static fPciDeviceCallback gPciDeviceCallback = NULL;

void PciSetDeviceCallback(fPciDeviceCallback fpdc){
    gPciDeviceCallback = fpdc;
}

static fPciBarChangeCallback gPciBarChangeCallback = NULL;
void PciRegisterBarChangeCallback(fPciBarChangeCallback callback){
    gPciBarChangeCallback = callback;
}

static void NotifyBarChange(uint8_t bus, uint8_t dev, uint8_t func,
                            int bar_index, uint64_t new_address){
    if(gPciBarChangeCallback){
        gPciBarChangeCallback(bus, dev, func, bar_index, new_address);
    }
}

//Core
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

    if(PciSimGetDiscoveryMode(bus, dev, func) && offset >=10 && offset < 0x28){
        int bar_index = (offset - 0x10) / 4;
        int is_high_dword = ((offset - 0x10) % 8) == 4 && offset > 0x10;
        if(is_high_dword){
            uint32_t low_val = *(uint32_t *)(PciSimGetConfig(bus, dev, func) + offset - 4);
            uint32_t type = (low_val >> 1) & 0x3;
            if(type == 2){
                int actual_bar = bar_index - 1;
                uint32_t mask = PciSimGetDiscoveryMaskHigh(bus, dev, func, actual_bar);
                if(mask != 0){
                    PciSimClearDiscovery(bus, dev, func);
                    return mask;
                }
            }
        }else{
            uint32_t mask = PciSimGetDiscoveryMaskLow(bus, dev, func, bar_index);
            if(mask != 0){
                PciSimClearDiscovery(bus, dev, func);
                return mask;
            }
        }
    }
    
    return *(uint32_t *)(PciSimGetConfig(bus, dev, func) + offset);
}

uint32_t PciConfigWrite32(uint8_t bus, uint8_t dev, uint8_t func,
                          uint8_t offset, uint32_t value){

    printf("[TRACE] PciConfigWrite32: bus=%d dev=%d func=%d offset=0x%02X value=0x%08X\n",
           bus, dev, func, offset, value);
    
    
    PCI_DEVICE *device = g_pci_bus[bus][dev][func];
    if(!device){
        LOG_WARN("[TRACE] Device not found!");
        return 0xffffffff;
    }
    uint8_t *begin_data = PciSimGetConfig(bus, dev, func);
    int is_high_dword = ((offset - 0x10) % 8 == 4) && (offset > 0x10);

    //command register (low_offset=0x04 and high_offset= 0x06)
    if((offset == 0x04) || ((offset >= 0x04) && (offset < 0x08))){
        uint16_t current_cmd = *(uint16_t *)(begin_data + 0x04);
        uint16_t new_cmd;
        if(offset == 0x04){
            new_cmd = value & 0xffff; //if value alter break anything store in a different variable and then compute;
        }else{
            //Todo: handle byte writes to 0x04 or 0x05 separately
            return 0;
        }
        //device-specific supported bits mask
        uint16_t supported_mask = PciSimGetDeviceCommandMask(device);
        uint16_t writeable_bits = supported_mask;

        //Reserved bits always read as 0, ignore write
        writeable_bits &= 0x07ff;

        new_cmd = ((current_cmd & (~writeable_bits)) | (new_cmd & writeable_bits));
        *(uint16_t *)(begin_data + 0x04) = new_cmd;

        uint32_t *cmd_status = (uint32_t *)(begin_data + 0x4);
        *cmd_status = (*cmd_status & 0xffff0000) | new_cmd;

        printf("[DEBUG]: PCI %02x:%02x.%x Command Register: 0x%04X -> 0x%04X [%s%s%s]\n",
                  bus, dev, func,
                  current_cmd, new_cmd,
                  (new_cmd & PCI_CMD_IO_SPACE) ? "IO " : "",
                  (new_cmd & PCI_CMD_MEMORY_SPACE) ? "MEM " : "",
                  (new_cmd & PCI_CMD_BUS_MASTER) ? "BUSMASTER" : "");

        return value;
    }
    int actual_bar;
    //bars
    if(offset >= 0x10 && offset < 0x28){
        /* Mapping config-space offset -> BAR index */
        int bar_index = (offset - 0x10) / 4;
        
        // Debug for bridge BAR2
        uint8_t header_type = (PciConfigRead32(bus, dev, func, 0x0C) >> 16) & 0x7F;
        if(header_type == 1 && bar_index >= 2){
            printf("DEBUG: Writing to bridge BAR%d (offset 0x%02X) value 0x%08X\n", 
                   bar_index, offset, value);
        }
                
        // int is_high_dword = ((offset - 0x10) % 8 == 4) && (offset > 0x10);
        int is_64bit = 0;
        actual_bar = bar_index;

        if(is_high_dword){
            uint32_t low_value = *(uint32_t *)(begin_data + offset - 4);
            uint32_t type = (low_value >> 1) & 0x3;
            if(type == 2){
                is_64bit = 1;
                actual_bar = bar_index - 1;
            }
        }else{
            uint32_t type = (*(uint32_t *)(begin_data + offset) >> 1) & 0x3;
            is_64bit = (type == 2);
        }

        uint64_t bar_size = PciSimGetBarSize(bus, dev, func, actual_bar);

        //Discovery: writing 0xffffffff
        if(value == 0xffffffff){
            if(bar_size == 0){
                return 0;
            }

            PciSimSetDiscoveryMode(bus, dev, func, 1);
            
            uint32_t orig = *(uint32_t *)(begin_data + offset);
            
            if(orig & 0x1){
                //IO BAR
                uint32_t mask = (uint32_t)(~(bar_size - 1)) & ~0x3;
                PciSimSetDiscoveryMaskLow(bus, dev, func, actual_bar, mask | (orig & 0x3) );
                PciSimSetDiscoveryMaskHigh(bus, dev, func, actual_bar, 0);
            }else{
                if(is_64bit){
                    uint64_t mask64 = ~(bar_size - 1);
                    uint32_t low_mask = (uint32_t)(mask64 & 0xffffffff);
                    uint32_t high_mask = (uint32_t)(mask64 >> 32);
                    PciSimSetDiscoveryMaskLow(bus, dev, func, actual_bar, (low_mask & ~0xf) | (orig & 0xf));
                    PciSimSetDiscoveryMaskHigh(bus, dev, func, actual_bar, high_mask);
                }else{
                    uint32_t mask = (uint32_t)(~(bar_size - 1)) & ~0xf;
                    PciSimSetDiscoveryMaskLow(bus, dev, func, actual_bar, mask | (orig & 0xf));
                    PciSimSetDiscoveryMaskHigh(bus, dev, func, actual_bar, 0);
                }
            }
            return 0;
        }

        PciSimClearDiscovery(bus, dev, func);

        //Apply hardware masking
        if(bar_size > 0 && !is_high_dword){
            uint32_t original_value = value;
            uint32_t size_32 = (uint32_t)bar_size;
            uint32_t orig_val = *(uint32_t *)(begin_data + offset);
            uint32_t type_bits = orig_val & 0xf;
            uint32_t size_mask = ~(size_32 - 1);
            uint32_t address_bits = value & size_mask;
            address_bits &= ~0xf;
            value = address_bits | type_bits;
            printf("[DEBUG] BAR write masking: size=0x%llx size_32=0x%08X mask=0x%08X wrote=0x%08X -> masked=0x%08X\n",
           bar_size, size_32, size_mask, original_value, value);
        }
    }
    *(uint32_t *)(PciSimGetConfig(bus, dev, func) + offset) = value;
    // VERIFY:
    uint32_t verify = *(uint32_t *)(PciSimGetConfig(bus, dev, func) + offset);
    if(offset >= 0x10 && offset < 0x28){
        printf("[DEBUG] BAR write verify: read back 0x%08X\n", verify);
        if(!is_high_dword){
            //Only notify on low dword; actual address change happens
            uint64_t new_addr = 0;
            uint32_t bar_low = *(uint32_t *)(begin_data + offset);
            if(bar_low & 0x1){
                new_addr = bar_low & ~0x3;
            }else{
                uint32_t type = (bar_low >> 1) & 0x3;
                if(type == 2){
                    uint32_t bar_high = *(uint32_t *)(begin_data + offset + 4);
                    new_addr = ((uint64_t)bar_high << 32) | (bar_low & ~0xf);
                }else{
                    new_addr = bar_low & ~0xf;
                }
            }
            NotifyBarChange(bus, dev, func, actual_bar, new_addr);
        }
    }
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

//Access bar memory and perform read and write and give access to upper layer through APIs chain
void PciSetBarAddress(uint8_t bus, uint8_t dev, uint8_t func,
                      int bar_index, uint64_t address){
    uint8_t offset = 0x10 + bar_index * 4;
    uint32_t orig = PciConfigRead32(bus, dev, func, offset);

    if(orig & 0x1){
        uint32_t io_addr = (uint32_t)(address & 0xfffffffC);
        uint32_t value = io_addr | (orig & 0x3);
        PciConfigWrite32(bus, dev, func, offset, value);
    }else{
        uint32_t type = (orig >> 1) & 0x3;
        if(type == 0x2){
            uint32_t low_addr = (uint32_t)(address & 0xfffffff0);
            uint32_t high_addr = (uint32_t)(address >> 32);
            uint32_t value = low_addr | (orig & 0xf);
            PciConfigWrite32(bus, dev, func, offset, value);
            value = high_addr;
            PciConfigWrite32(bus, dev, func, offset + 4, value);
        }else{
            uint32_t mem_addr = (uint32_t)(address & 0xfffffff0);
            uint32_t value = mem_addr | (orig & 0xf);
            PciConfigWrite32(bus, dev, func, offset, value);
        }
    }
}

void* PciGetBarMemory(uint8_t bus, uint8_t dev, uint8_t func,
                      int bar_index){
    return PciSimGetBarMemory(bus, dev, func, bar_index);
}

uint64_t PciReadBar(uint8_t bus, uint8_t dev, uint8_t func,
                    int bar_index, uint64_t offset, int size){
    return PciSimReadBar(bus, dev, func, bar_index, offset, size);
}

void PciWriteBar(uint8_t bus, uint8_t dev, uint8_t func,
                 int bar_index, uint64_t offset, uint64_t value, int size){
    PciSimWriteBar(bus, dev, func, bar_index, offset, value, size);
}

//cmd register
// 0 means false and 1 means true
int PciValidateAccess(uint8_t bus, uint8_t dev, uint8_t func,
                      uint64_t address, int is_io){
    PCI_DEVICE *device = g_pci_bus[bus][dev][func];
    if(!device){
        LOG_WARN("PciValidateAccess: Device not found");
        return 0;
    } 
    
    uint8_t *begin_data = PciSimGetConfig(bus, dev, func);
    uint16_t command = *(uint16_t *)(begin_data + 0x04);

    printf("[DEBUG] PciValidateAccess: addr=0x%llx, is_io=%d, cmd=0x%04X\n", 
           address, is_io, command);

    if(is_io){
        if(!(command & PCI_CMD_IO_SPACE)){
            printf("[DEBUG]: Device %02x:%02x.%x: I/O space disabled, ignoring access\n",
                   bus, dev, func);
            return 0;
        }
    }else{
        if(!(command & PCI_CMD_MEMORY_SPACE)){
            printf("[DEBUG]: Device %02x:%02x.%x: I/O space disabled, ignoring access\n",
                   bus, dev, func);
            return 0; 
        }
    }

    LOG_INFO("Space enabled, checking BARs...");
    
    for(int i = 0; i < 6; i++){
        uint64_t bar_size = PciSimGetBarSize(bus, dev, func, i);
        printf("[DEBUG] BAR%d: size=0x%llx\n", i, bar_size);
        if(bar_size == 0) continue;
        
        uint32_t *bar_ptr = (uint32_t *)(begin_data + 0x10 + i * 4);
        uint32_t bar_raw = *bar_ptr;
        uint64_t bar_addr = bar_raw & ~0xf;

        printf("[DEBUG] BAR%d: raw=0x%08X, addr=0x%llx\n", i, bar_raw, bar_addr);
            
        if((bar_raw & 0x7) == 0x4){
            bar_addr |= ((uint64_t)*(bar_ptr + 1)) << 32;
            printf("[DEBUG] BAR%d: 64-bit, upper=0x%08X, full_addr=0x%llx\n", 
                   i, *(bar_ptr + 1), bar_addr);
            i++;
        }
    
        if(bar_addr == 0){
            printf("[DEBUG] BAR%d: address is 0, skipping\n", i);
            continue;
        }
        printf("[DEBUG] BAR%d: checking 0x%llx in [0x%llx - 0x%llx]\n",
               i, address, bar_addr, bar_addr + bar_size - 1);

        if((address >= bar_addr) && (address < bar_addr + bar_size)){
            printf("[DEBUG] BAR%d: size=0x%llx\n", i, bar_size);
            return 1;
        }
    }
    printf("[DEBUG] No BAR matched address 0x%llx\n", address);
    return 0;
}

int PciBridgeValidateForward(uint8_t bus, uint8_t dev, uint8_t func,
                            uint64_t address, int is_io){
    PCI_DEVICE *device = g_pci_bus[bus][dev][func];
    if(!device) return 0;

    uint8_t *begin_data = PciSimGetConfig(bus, dev, func);
    uint8_t header_type = (*(uint8_t *)(begin_data + 0x0e)) & 0x7f;

    printf("[DEBUG] BridgeValidateForward: bus=%d dev=%d func=%d addr=0x%llx header=%d\n",
           bus, dev, func, address, header_type);
    
    if(header_type != 1){
        printf("[DEBUG] Not a bridge (header=%d)\n", header_type);
        return 0;
    }

    uint16_t command = *(uint16_t *)(begin_data + 0x04);
    printf("[DEBUG] Bridge command: 0x%04X\n", command);
    
    if(!(command & PCI_CMD_BUS_MASTER)){
        printf("[DEBUG]: Device %02x:%02x.%x: I/O space disabled, ignoring access\n",
               bus, dev, func);
        return 0;
    }
    
    if(is_io){
        if(!(command & PCI_CMD_IO_SPACE)){
            printf("[DEBUG]: Device %02x:%02x.%x: I/O space disabled, ignoring access\n",
                   bus, dev, func);
            return 0;
        }
        
        uint8_t io_base = *(uint8_t *)(begin_data + 0x1c);
        uint8_t io_limit = *(uint8_t *)(begin_data + 0x1d);

        printf("[DEBUG] I/O window raw: base=0x%02X limit=0x%02X\n", io_base, io_limit);
        if(io_base == 0 || io_limit == 0){
            LOG_INFO("[DEBUG] I/O window not programmed");
            return 0;
        } 
        
        uint16_t io_addr = address & 0xffff;
        uint16_t window_base = (io_base & 0xf0) << 8;
        uint16_t window_limit = ((io_limit & 0xf0) << 8) | 0xfff;

        printf("[DEBUG] I/O window: 0x%04X - 0x%04X, checking addr 0x%04X\n",
               window_base, window_limit, io_addr);

        if(io_addr >= window_base && io_addr <= window_limit){
            LOG_INFO("[DEBUG] Address IN I/O window!\n");
            return 1;
        }
        LOG_INFO("[DEBUG] Address NOT in I/O window\n");
        return 0;
    }
    else{
        if(!(command & PCI_CMD_MEMORY_SPACE)){
            printf("[DEBUG]: Device %02x:%02x.%x: I/O space disabled, ignoring access\n",
                   bus, dev, func);
            return 0; 
        }

        uint32_t mem_window = *(uint32_t *)(begin_data + 0x20);
        uint16_t mem_base = mem_window & 0xffff;
        uint16_t mem_limit = (mem_window >> 16) & 0xffff;

       printf("[DEBUG] Memory window raw: 0x%08X -> base=0x%04X limit=0x%04X\n",
               mem_window, mem_base, mem_limit);
        
        if(mem_base != 0 || mem_limit != 0){
            uint64_t window_base = (uint64_t)(mem_base & 0xfff0) << 16;
            uint64_t window_limit = ((uint64_t)(mem_limit & 0xfff0) << 16) | 0xfffff;

            printf("[DEBUG] Memory window: 0x%" PRIx64 " - 0x%" PRIx64 ", checking addr 0x%" PRIx64 "\n",
                   window_base, window_limit, address);
        
            if(address >= window_base && address <= window_limit){
                LOG_INFO("[DEBUG] Address IN memory window!\n");
                return 1;
            }
        }else{
            printf("[DEBUG] Memory window not programmed (base=0x%04X, limit=0x%04X)\n",
                   mem_base, mem_limit);
        }

        uint16_t pref_base = *(uint16_t *)(begin_data + 0x24);
        uint16_t pref_limit = *(uint16_t *)(begin_data + 0x26);

        printf("[DEBUG] Prefetch window raw: base=0x%04X limit=0x%04X\n",
               pref_base, pref_limit);
        
        if(pref_base != 0 && pref_limit != 0){
            uint64_t pref_window_base = (pref_base & 0xfff0) << 16;
            uint64_t pref_window_limit = ((pref_limit & 0xfff0) << 16) | 0xfffff;

            //check for 64-bit prefetchable support
            uint32_t pref_base_upper = *(uint32_t *)(begin_data + 0x28);
            uint32_t pref_limit_upper = *(uint32_t *)(begin_data + 0x2c);
            
            if(pref_base_upper != 0 || pref_limit_upper != 0){
                pref_window_base |= (uint64_t)pref_base_upper << 32;
                pref_window_limit |= (uint64_t)pref_limit_upper << 32;
                printf("[DEBUG] 64-bit prefetch window: 0x%" PRIx64 " - 0x%" PRIx64 "\n",
                       pref_window_base, pref_window_limit);
            }else{
                printf("[DEBUG] 32-bit prefetch window: 0x%" PRIx64 " - 0x%" PRIx64 "\n",
                       pref_window_base, pref_window_limit);
            }
            if(address >= pref_window_base && address <= pref_window_limit){
                LOG_INFO("[DEBUG] Address IN prefetch window!\n");
                return 1;
            }    
        }else{
            LOG_INFO("[DEBUG] Prefetch window not programmed\n");
        }
    }                   
    printf("[DEBUG] Address 0x%" PRIx64 " NOT in any bridge window\n", address);
    return 0;
}

//To do: func for 0x80 need check for host controller
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

            if(gPciDeviceCallback){
                gPciDeviceCallback(bus, dev, func);
            }
            
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
