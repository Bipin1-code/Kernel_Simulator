
#include "pci_sim.h"
#include "pci_sim_internal.h"
#include "log.h"
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#define MAX_BUS 2   //256 in real but for test it's 2
#define MAX_DEVICE 32
#define MAX_FUNCTION 8

#pragma pack(push, 1)
typedef struct _PCI_CONFIG_SPACE{
    uint8_t data[256];
} PCI_CONFIG_SPACE;

typedef struct _PCI_COMMON_HEADER{
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t command;
    uint16_t status;
    uint8_t revision_id;
    uint8_t prog_if;
    uint8_t subclass;
    uint8_t class_code;
    uint8_t cache_line_size;
    uint8_t latency_timer;
    uint8_t header_type;
    uint8_t bist;
} PCI_COMMON_HEADER;

typedef struct _PCI_HEADER_TYPE0{
    uint32_t bar[6];
    uint32_t carbus_cis_ptr;
    uint16_t subsystem_vendor_id;
    uint16_t subsystem_id;
    uint32_t expansion_rom_base;
    uint8_t capabilities_ptr;
    uint8_t reserved1[3];
    uint32_t reserved2;
    uint8_t interrupt_line;
    uint8_t interrupt_pin;
    uint8_t min_grant;
    uint8_t max_latency;
} PCI_HEADER_TYPE0;

typedef struct _PCI_HEADER_TYPE1{
    uint32_t bar[2];
    uint8_t primary_bus;
    uint8_t secondary_bus;
    uint8_t subordinate_bus;
    uint8_t secondary_latency;
    uint8_t io_base;
    uint8_t io_limit;
    uint16_t secondary_status;
    uint16_t memory_base;
    uint16_t memory_limit;
    uint16_t prefetch_base;
    uint16_t prefetch_limit;
    uint32_t prefetch_base_upper32;
    uint32_t prefetch_limit_upper32;
    uint16_t io_base_up16;
    uint16_t io_limit_up16;
    uint8_t capability_ptr;
    uint8_t reserved[3];
    uint32_t expansion_rom_base;
    uint8_t interrupt_line;
    uint8_t interrupt_pin;
    uint16_t bridge_control;
} PCI_HEADER_TYPE1;

typedef struct _PCI_DEVICE{
    PCI_CONFIG_SPACE config;
    uint64_t bar_size[6]; //hardware internal address decode circuitry mimic
    uint8_t *bar_mem[6];  //this is the actual memory base address
} PCI_DEVICE;
#pragma pack(pop)

PCI_DEVICE *g_pci_bus[MAX_BUS][MAX_DEVICE][MAX_FUNCTION];

//start
//this struct is only for to help initialisation
//this is not PCI Device expose part for OS or BIOS/UFEI
typedef struct _BAR_CFG{
    uint64_t size;
    uint8_t is_io;
    uint8_t is_64;
} BAR_CFG;
//end

static PCI_DEVICE* CreatePciDevice(const char *name){
    PCI_DEVICE *device = malloc(sizeof(PCI_DEVICE));
    if(!device){
        LOG_ERROR_FMT("Failed to initialize PCI_DEVICE %s", name);
        return NULL;
    }
    for(int byte = 0; byte < 256; byte++)
        device->config.data[byte] = 0;

    for(int i = 0; i < 6; i++)
        device->bar_size[i] = 0;
    
    return device;
}

//Random ID generator (not part of real world)
static uint16_t GenerateRandomID(const char *name){
    uint32_t hash = 5381;
    while (*name) {
        hash = ((hash << 5) + hash) + *name; // djb2
        name++;
    }
    return (uint16_t)(hash & 0xFFFF);
}

static void SetupCommonHeader(PCI_DEVICE *device, uint16_t id, uint8_t type,
                       uint8_t subc, uint8_t cc, uint8_t pf){
    uint8_t *data = device->config.data;
    PCI_COMMON_HEADER *common = (PCI_COMMON_HEADER *)data;
    
    common->vendor_id = id;
    common->device_id = ((id * 13) & 0xffff);
    common->command = 0x00;
    common->status = 0x00;
    common->revision_id = (id & 0xff);
    common->prog_if = pf;
    common->subclass = subc;
    common->class_code = cc;
    common->cache_line_size = 0x10;
    common->latency_timer = 0x00;
    common->header_type = type;
    common->bist = 0x00;

    KASSERT(sizeof(PCI_COMMON_HEADER) == 16, "Common header broken");
}

static void SetupType0Header(PCI_DEVICE *device, uint16_t id, BAR_CFG *bars){
    uint8_t *data = device->config.data;
    PCI_HEADER_TYPE0 *type = (PCI_HEADER_TYPE0 *)(data + 0x10);
    
    for(int i = 0; i < 6; i++){
        if(bars[i].size == 0){
            type->bar[i] = 0;
            device->bar_size[i] = 0;
            device->bar_mem[i] = NULL;
            continue;
        }
        device->bar_size[i] = bars[i].size;
        device->bar_mem[i] = malloc(bars[i].size);
        memset(device->bar_mem[i], 0, bars[i].size);

        if(!device->bar_mem[i]){
            LOG_ERROR_FMT("Failed to allocate BAR%d memory", i);
            return;
        }

        uint32_t flags = 0;
        if(bars[i].is_io){
            flags |= 0x1;
            // IO BAR cannot be 64-bit
            KASSERT(!bars[i].is_64, "IO BAR cannot be 64-bit");
        }else{
            if(bars[i].is_64)
                flags |= (2 << 1);
        }

        type->bar[i] = flags;
        
        //for 64-bit BAR, handling
        if(bars[i].is_64){
            i++;
            type->bar[i] = 0;
        }
    }

    type->carbus_cis_ptr = 0x00; 
    type->subsystem_vendor_id = id;
    type->subsystem_id = id;
    type->expansion_rom_base = 0x00;
    type->capabilities_ptr = 0x00;
    
    for(int r = 0; r < 3; r++)
        type->reserved1[r] = 0;
    
    type->reserved2 = 0x00;
    type->interrupt_line = 0x00;
    type->interrupt_pin = 0x01;
    type->min_grant =  0x00;
    type->max_latency = 0x00;
    
    KASSERT(sizeof(PCI_HEADER_TYPE0) == 48, "Type0 broken");
}

static void SetupType1Header(PCI_DEVICE *device, uint16_t mmL, BAR_CFG *bars){
    uint8_t *data = device->config.data;
    PCI_HEADER_TYPE1 *type = (PCI_HEADER_TYPE1 *)(data + 0x10);

    for(int i = 0; i < 2; i++){
        if(bars[i].size == 0){
            type->bar[i] = 0;
            device->bar_size[i] = 0;
            device->bar_mem[i] = NULL;
            continue;
        }
        device->bar_size[i] = bars[i].size;
        device->bar_mem[i] = malloc(bars[i].size);
        memset(device->bar_mem[i], 0, bars[i].size);
        
        if(!device->bar_mem[i]){
            LOG_ERROR_FMT("Failed to allocate BAR%d memory", i);
            return;
        }
        
        uint32_t flags = 0;
        if(bars[i].is_io){
            flags |= 0x1;
            // IO BAR cannot be 64-bit
            KASSERT(!bars[i].is_64, "IO BAR cannot be 64-bit");
        }else{
            if(bars[i].is_64)
                flags |= (2 << 1);
        }
        type->bar[i] = flags;
        
        if(bars[i].is_64){
            i++;
            type->bar[i] = 0;
        }
    }
    
    type->primary_bus = 0x00;
    type->secondary_bus = 1;
    type->subordinate_bus = 1;
    type->secondary_latency = 0x00;
    type->io_base = 0x00;
    type->io_limit = 0x00;
    type->secondary_status = 0x0000;
    type->memory_base = 0x0000;
    type->memory_limit = mmL;
    type->prefetch_base = 0;
    type->prefetch_limit = 0;
    type->prefetch_base_upper32 = 0;
    type->prefetch_limit_upper32 = 0;
    type->io_base_up16 = 0;
    type->io_limit_up16 = 0;
    type->capability_ptr = 0x00;
    type->expansion_rom_base = 0x00;
    type->interrupt_line = 0x00;
    type->interrupt_pin = 0x01;
    type->bridge_control = 0x0000;
    KASSERT(sizeof(PCI_HEADER_TYPE1) == 48, "Type1 broken");
}

static PCI_DEVICE* CreateFakeDevice(const char *name, uint8_t type, uint8_t subclass,
                             uint8_t classcode, uint8_t prog_if, uint16_t mmL,
                             BAR_CFG *bars){
    PCI_DEVICE *device = CreatePciDevice(name);
    if(!device){
        LOG_ERROR_FMT("Failed to initialize PCI_DEVICE %s", name);
        return NULL;
    }
    uint16_t id = GenerateRandomID(name);
    SetupCommonHeader(device, id, type, subclass, classcode, prog_if);
    if(type == 0){
        (void)mmL;
        SetupType0Header(device, id, bars);
    }else
        SetupType1Header(device, mmL, bars);
    
    return device;
}

//These below functions are APIs for upper layers
uint8_t* PciSimGetConfig(uint8_t bus, uint8_t dev, uint8_t func){
    PCI_DEVICE *device = g_pci_bus[bus][dev][func];
    if(!device) return NULL;
    return device->config.data;
}

uint64_t PciSimGetBarSize(uint8_t bus, uint8_t dev, uint8_t func,
                          int bar_index){
    PCI_DEVICE *device = g_pci_bus[bus][dev][func];
    if(!device) return 0;
    if(bar_index < 0 || bar_index >= 6)
        return 0;

    return device->bar_size[bar_index];
}

void* PciSimGetBarMemory(uint8_t bus, uint8_t dev, uint8_t func,
                         int bar_index){
    PCI_DEVICE *device = g_pci_bus[bus][dev][func];
    if(!device) return NULL;
    if(bar_index < 0 || bar_index >= 6) return NULL;
    return device->bar_mem[bar_index];
}

uint64_t PciSimReadBar(uint8_t bus, uint8_t dev, uint8_t func,
                       int bar_index, uint64_t offset, int size){
    uint8_t *mem = PciSimGetBarMemory(bus, dev, func, bar_index);
    uint64_t bar_size = PciSimGetBarSize(bus, dev, func, bar_index);

    if(!mem || ((offset + size) > bar_size)){
        LOG_ERROR_FMT("BAR read out of bounds:bar=%d, offset=0x%" PRIx64 ", size=%d",
                      bar_index, offset, size);
        return 0;
    }
    switch(size){
        case 1: return *(uint8_t *)(mem + offset);
        case 2: return *(uint16_t *)(mem + offset);
        case 4: return *(uint32_t *)(mem + offset);
        case 8: return *(uint64_t *)(mem + offset);
        default:
            LOG_ERROR_FMT("INVALID read size: %d", size);
            return 0;
    }

}

void PciSimWriteBar(uint8_t bus, uint8_t dev, uint8_t func,
                    int bar_index, uint64_t offset, uint64_t value, int size){
    uint8_t *mem = PciSimGetBarMemory(bus, dev, func, bar_index);
    uint64_t bar_size = PciSimGetBarSize(bus, dev, func, bar_index);

    if(!mem || ((offset + size) > bar_size)){
        LOG_ERROR_FMT("BAR write out of bounds: bar=%d, offset=0x%" PRIx64 ", size=%d",
                      bar_index, offset, size);
        return;
    }
    switch(size){
        case 1:
            *(uint8_t *)(mem + offset) = (uint8_t)value;
            break;
        case 2:
            *(uint16_t *)(mem + offset) = (uint16_t)value;
            break;
        case 4:
            *(uint32_t *)(mem + offset) = (uint32_t)value;
            break;
        case 8:
            *(uint64_t *)(mem + offset) = value;
            break;
        default:
            LOG_ERROR_FMT("INVALID write size: %d", size);
    }
}

void PciDevicePool(){
    //Test Devices 
    // -------- Disk Controller (AHCI-like) --------
    BAR_CFG disk_bars[6] = {0};
    disk_bars[0] = (BAR_CFG){0x2000, 0, 0};  // 32-bit MMIO (8KB)

    // -------- Network Card --------
    BAR_CFG net_bars[6] = {0};
    net_bars[0] = (BAR_CFG){0x4000, 0, 1};  // 64-bit MMIO 
    // BAR1 automatically consumed (must stay zero)

    // -------- GPU --------
    BAR_CFG gpu_bars[6] = {0};
    gpu_bars[0] = (BAR_CFG){0x100000, 0, 1}; // 64-bit MMIO 
    gpu_bars[2] = (BAR_CFG){0x1000, 1, 0};   // IO BAR 

    // -------- PCI Bridge --------
    BAR_CFG bridge_bars[2] = {0};
    bridge_bars[0] = (BAR_CFG){0x4000, 0, 0}; // 32-bit MMIO

    // -------- Install devices --------
    g_pci_bus[0][0][0] = CreateFakeDevice(
        "disk",   // name
        0,        // header type
        0x06,     // subclass 
        0x01,     // class 
        0x01,     // prog IF 
        0,
        disk_bars
    );

    g_pci_bus[0][1][0] = CreateFakeDevice(
        "net",
        0,
        0x00,     // subclass (ethernet)
        0x02,     // class (network)
        0x00,
        0,
        net_bars
    );

    g_pci_bus[1][0][0] = CreateFakeDevice(
        "gpu",
        0,
        0x00,     // subclass (VGA)
        0x03,     // class (display)
        0x00,
        0,
        gpu_bars
    );

    g_pci_bus[0][2][0] = CreateFakeDevice(
        "bridge",
        1,        // header type (bridge)
        0x04,     // subclass (PCI-to-PCI bridge)
        0x06,     // class (bridge device)
        0x00,
        0,
        bridge_bars
    );
}
