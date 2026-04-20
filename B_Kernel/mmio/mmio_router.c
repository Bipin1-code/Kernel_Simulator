
#include "mmio_router.h"
#include "OS_device.h"
#include "pci_logic.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <inttypes.h>

extern PCI_DEVICE_CONTEXT *g_pciDevCtx[];
extern int g_dev_count;

//Private Functions
static bool IsDeviceEnabled(PCI_DEVICE_CONTEXT *ctx, bool is_io_space){
    uint32_t cmd = PciConfigRead32(ctx->bus, ctx->dev, ctx->func, 0x04);
    uint16_t command = cmd & 0xffff;
    if(is_io_space){
        bool enabled = (command & 0x0001) != 0; //I/O Space Enable
        printf("[MMIO]     Command=0x%04X, I/O enable=%d\n", command, enabled);
        return enabled;
    }else{
        bool enabled = (command & 0x0002) != 0; //Memory Space Enable
        printf("[MMIO]     Command=0x%04X, Memory enable=%d\n", command, enabled);

        return enabled;
    }
}

static bool EndpointClaimsAddress(PCI_DEVICE_CONTEXT *ctx, uint64_t phys_addr,
                                  bool is_io_space, MMIO_MATCH *match){
    for(int bar = 0; bar < 6; bar++){
        PCI_BAR_INFO *info = &ctx->bar_info[bar];

        printf("[MMIO]         BAR%d: base=0x%llx size=0x%llx is_io=%d\n",
               bar, info->base_address, info->size, info->is_io);
        
        //skip unprogrammed BARs
        if(info->size == 0){
            printf("[MMIO]           size = 0 - skip\n");
            continue;
        }
        if(info->base_address == 0){
            printf("[MMIO]           base=0 - skip\n");
            continue;
        }
        if(is_io_space && !info->is_io){
            printf("[MMIO]           wrong space type - skip\n");
            continue;
        }
        if(!is_io_space && info->is_io){
            printf("[MMIO]           wrong space type - skip\n");
            continue;
        }
        //Hardware comparison
        uint64_t mask = ~(info->size - 1);
        uint64_t bar_masked = (info->base_address & mask);
        uint64_t addr_masked = (phys_addr & mask);
        
        printf("[MMIO]           mask=0x%llx bar_masked=0x%llx addr_masked=0x%llx\n",
               mask, bar_masked, addr_masked);
        
        if(addr_masked == bar_masked){
            match->bus = ctx->bus;
            match->dev = ctx->dev;
            match->func = ctx->func;
            match->bar_index = bar;
            match->bar_base = info->base_address;
            match->bar_size = info->size;
            match->offset = phys_addr & (info->size - 1);
            match->is_io_space = info->is_io;
            match->is_64bit = info->is_64bit;
            return true;
        }   
    }
    return false;
}

static bool AddressInMemoryWindow(PCI_DEVICE_CONTEXT *bridge, uint64_t phys_addr){
    //Memory window at offset 0x24-0x27
    uint32_t mem_window = PciConfigRead32(bridge->bus, bridge->dev, bridge->func,
                                          0x20);
    uint16_t mem_base = mem_window & 0xffff;
    uint16_t mem_limit = (mem_window >> 16) & 0xffff;
    if(mem_base == 0 || mem_limit == 0){
        return false;
    }
    uint64_t window_base = (uint64_t)(mem_base & 0xfff0) << 16;
    uint64_t window_limit = ((uint64_t)(mem_limit & 0xfff0) << 16) | 0xfffff;
    printf("[MMIO] Memory window: 0x%" PRIx64 "- 0x%" PRIx64 "\n",
           window_base, window_limit);

    return (phys_addr >= window_base && phys_addr <= window_limit);
}

static bool AddressInPrefetchWindow(PCI_DEVICE_CONTEXT *bridge, uint64_t phys_addr){
    //Prefetchable memory window at offset 0x24-0x27
    uint32_t pref_window = PciConfigRead32(bridge->bus, bridge->dev, bridge->func,
                                          0x24);
    uint16_t pref_base = pref_window & 0xffff;
    uint16_t pref_limit = (pref_window >> 16) & 0xffff;
    if(pref_base == 0 || pref_limit == 0){
        return false;
    }
    uint64_t window_base = (uint64_t)(pref_base & 0xfff0) << 16;
    uint64_t window_limit = ((uint64_t)(pref_limit & 0xfff0) << 16) | 0xfffff;

    //check for 64-bt
    uint32_t pref_base_upper = PciConfigRead32(bridge->bus, bridge->dev, bridge->func,
                                               0x28);
    if(pref_base_upper != 0){
        //64-bit
        window_base |= (uint64_t)pref_base_upper << 32;
        uint32_t pref_limit_upper = PciConfigRead32(bridge->bus, bridge->dev, bridge->func,
                                                    0x2c);
        window_limit |= (uint64_t)pref_limit_upper << 32;
        printf("[MMIO] 64-bit Prefetch window: 0x%" PRIx64 " - 0x%" PRIx64 "\n",
               window_base, window_limit);
    }else{
        printf("[MMIO] Prefetch window: 0x%" PRIx64 " - 0x%" PRIx64 "\n",
               window_base, window_limit);
    }
    return (phys_addr >= window_base && phys_addr <= window_limit); 
}

static bool AddressInIoWindow(PCI_DEVICE_CONTEXT *bridge, uint64_t phys_addr){
    //Memory window at offset 0x24-0x27
    uint16_t io_window = PciConfigRead32(bridge->bus, bridge->dev, bridge->func,
                                          0x1c) & 0xffff;
    uint8_t io_base = io_window & 0xff;
    uint8_t io_limit = (io_window >> 8) & 0xff;
    if(io_base == 0 || io_limit == 0){
        return false;
    }
    uint16_t io_addr = phys_addr & 0xffff;
    uint16_t window_base = (io_base & 0xf0) << 8;
    uint16_t window_limit = ((io_limit & 0xf0) << 8) | 0xfff;
    printf("[MMIO] I/O window: 0x%04X - 0x%04X, checking io_addr=0x%04X\n",
           window_base, window_limit, io_addr);

    return (io_addr >= window_base && io_addr <= window_limit);

}

static bool BridgeForwardsAddress(PCI_DEVICE_CONTEXT *bridge, uint64_t phys_addr,
                                  bool is_io_space, MMIO_MATCH *match);

static bool ScanBusForDevice(uint8_t bus, uint64_t phys_addr, bool is_io_space,
                             MMIO_MATCH *match){
     printf("[MMIO] Scanning bus %d for addr=0x%" PRIx64 "\n", bus, phys_addr);

     //scaning all devices on this bus
     for(int i = 0; i < g_dev_count; i++){
         PCI_DEVICE_CONTEXT *ctx = g_pciDevCtx[i];
         if(!ctx) continue;
         if(ctx->bus != bus) continue;
         
         printf("[MMIO]   Checking device %02x:%02x.%x (header=%d)\n", 
               ctx->bus, ctx->dev, ctx->func, ctx->header_type);

         if(!IsDeviceEnabled(ctx, is_io_space)){
             printf("[MMIO] Device disabled - skipping\n");
             continue;
         }       
         
         if(ctx->header_type == 1){
             printf("[MMIO] THIS IS A BRIDGE\n");
             //Nested bridge -recursively check 
             if(BridgeForwardsAddress(ctx, phys_addr, is_io_space, match))
                 return true;
             
             if(EndpointClaimsAddress(ctx, phys_addr, is_io_space, match)){
                 printf("[MMIO Bridge BAR claimed!\n]");
                 return true;
             }
         }else{
             if(EndpointClaimsAddress(ctx, phys_addr, is_io_space, match)){
                 printf("[MMIO Endpoint BAR claimed!\n]");
                 return true;
             }
         }
     }
     return false;
}

static bool BridgeForwardsAddress(PCI_DEVICE_CONTEXT *bridge, uint64_t phys_addr,
                                  bool is_io_space, MMIO_MATCH *match){
    printf("[MMIO] Checking bridge at %02x:%02x.%x for addr=0x%" PRIx64 "\n",
           bridge->bus, bridge->dev, bridge->func, phys_addr);
    uint8_t secondary_bus = 0;
    uint8_t subordinate_bus = 0;
    uint32_t bus_numbers = PciConfigRead32(bridge->bus, bridge->dev, bridge->func, 0x18);
    secondary_bus = (bus_numbers >> 8) & 0xff;
    subordinate_bus = (bus_numbers >> 16) & 0xff;
    printf("[MMIO] Bridge secondary=%d subordinate=%d\n",
           secondary_bus, subordinate_bus);

    if(secondary_bus == 0){
        printf("[MMIO] Bridge not configured (secondary bus = 0)\n");
        return false;
    }

    bool in_window = false;
    if(is_io_space){
        in_window = AddressInIoWindow(bridge, phys_addr);
    }else{
        in_window = AddressInMemoryWindow(bridge, phys_addr) ||
            AddressInPrefetchWindow(bridge, phys_addr);
    }

    if(!in_window){
        printf("[MMIO] Address not in any bridge window\n");
        return EndpointClaimsAddress(bridge, phys_addr, is_io_space,
                                     match);
    }
    printf("[MMIO] Address in bridge window - forwarding to bus %d\n", secondary_bus);

    //forward to secondary bus - scall all devices on that bus
    for(uint8_t bus = secondary_bus; bus <= subordinate_bus; bus++){
        if(ScanBusForDevice(bus, phys_addr, is_io_space, match)){
            printf("[MMIO] Found device on forwarded bus %d\n", bus);
            return true;
        }
    }
    printf("[MMIO] No device claimed address on forwarded buses\n");
    return false;
}

//APIs for users
MMIO_MATCH* MmioFindDevice(uint64_t phys_addr, bool is_io_space){
    MMIO_MATCH *match = (MMIO_MATCH *)calloc(1, sizeof(MMIO_MATCH));
    if(!match) return NULL;

    printf("[MMIO] Finding device for addr=0x%" PRIx64 ", space=%s\n",
           phys_addr, is_io_space ? "I/O" : "Memory");

    if(ScanBusForDevice(0, phys_addr, is_io_space, match)){
        printf("[MMIO] Found! Bus=%d Dev=%d Func=%d BAR=%d offset=0x%" PRIx64 "\n",
               match->bus, match->dev, match->func,
               match->bar_index, match->offset);
        return match;
    }  
    
    printf("[MMIO] No device claimed address 0x%" PRIx64 "\n",
           phys_addr);
    free(match);
    return NULL;
}

void MmioPrintMatch(MMIO_MATCH *match){
    if(!match){
        printf("[MMIO] NULL match\n");
        return;
    }
    printf("[MMIO] MATCH: Bus=%d Dev=%d Func=%d BAR=%d\n",
           match->bus, match->dev, match->func, match->bar_index);
    
    printf("[MMIO]    Base=0x%" PRIx64 " Size=0x%" PRIx64 " Offset=0x%" PRIx64 "\n",
           match->bar_base, match->bar_size, match->offset);

    printf("[MMIO]   Space=%s 64-bit=%s\n",
           match->is_io_space ? "I/O" : "Memory",
           match->is_64bit ? "Yes" : "No");
}

uint64_t MmioRouteRead(uint64_t phys_addr, int size_bytes){
    printf("[MMIO] READ: addr=0x%" PRIx64 " size=%d\n",
           phys_addr, size_bytes);
    MMIO_MATCH  *match = MmioFindDevice(phys_addr, false);
    if(!match){
        //PCI spec: reads to unmapped space return all 1s
        printf("[MIMO] No device, returning all 1s\n");
        return (size_bytes == 1) ? 0xff :
            (size_bytes == 2) ? 0xffff :
            (size_bytes == 4) ? 0xffffffff: 0xffffffffffffffffULL;
    }

    PCI_DEVICE_CONTEXT *ctx = NULL;
    for(int i = 0; i < g_dev_count; i++){
        if(g_pciDevCtx[i]->bus == match->bus &&
           g_pciDevCtx[i]->dev == match->dev &&
           g_pciDevCtx[i]->func == match->func){
            ctx = g_pciDevCtx[i];
            break;
        }
    }
    //Perform the read
    uint64_t result = 0;
    if(ctx && ctx->fBarRead){
        result = ctx->fBarRead(ctx, match->bar_index, match->offset,
                                 size_bytes);
    }
    
    printf("[MMIO] READ result=0x%" PRIx64 "\n", result);
    free(match);
    return result;
}

void MmioRouteWrite(uint64_t phys_addr, uint64_t value, int size_bytes){
    printf("[MMIO] WRITE : addr=0x%" PRIx64 " value=0x%" PRIX64 " size=%d\n",
           phys_addr, value, size_bytes);
    MMIO_MATCH *match = MmioFindDevice(phys_addr, false);

    if(!match){
        printf("[MMIO] No device, write dropped\n");
        return;
    }

    PCI_DEVICE_CONTEXT *ctx = NULL;
    for(int i = 0; i < g_dev_count; i++){
        if(g_pciDevCtx[i]->bus == match->bus &&
           g_pciDevCtx[i]->dev == match->dev &&
           g_pciDevCtx[i]->func == match->func){
            ctx = g_pciDevCtx[i];
            break;
        }
    }

    if(ctx && ctx->fBarWrite){
        ctx->fBarWrite(ctx, match->bar_index, match->offset,
                       value, size_bytes);
    }

    printf("[MMIO] WRITE completed\n");
    free(match);
}


