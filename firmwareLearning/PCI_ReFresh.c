//Refresh PCI:  improve the foundation design which I learn but hit the wall

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>

//Log each output
#define LOG_INFO(msg) \
    printf("\033[32m[INFO]\033[0m[%s:%d] %s\n", __FILE__, __LINE__, msg)
#define LOG_INFO_FMT(fmt, ...) \
    printf("\033[32m[INFO]\033[0m[%s:%d] " fmt "\n", __FILE__, __LINE__, __VA_ARGS__)

//WARN which potentially could give misleading answer but doesn't break program
#define LOG_WARN(msg) \
    printf("\033[33m[WARN]\033[0m[%s:%d] %s\n", __FILE__, __LINE__, msg)
#define LOG_WARN_FMT(fmt, ...) \
    printf("\033[33m[WARN]\033[0m[%s:%d] " fmt "\n", __FILE__, __LINE__, __VA_ARGS__)

//ERROR which determine deterministic error in some computation    
#define LOG_ERROR(msg) \
    printf("\033[31m[ERROR]\033[0m[%s:%d] %s\n", __FILE__, __LINE__, msg)
#define LOG_ERROR_FMT(fmt, ...) \
    printf("\033[31m[ERROR]\033[0m[%s:%d] " fmt "\n", __FILE__, __LINE__, __VA_ARGS__)

//Absolute Invariant violation
#define PANIC(msg)\
    do{ \
        printf("\n\x1b[31m[PANIC]\x1b[0m[%s:%d] %s\n", __FILE__, __LINE__, msg); \
        exit(1); \
    } while(0)

#define KASSERT(cond, msg) \
    do{ \
        if(!(cond)){ \
            PANIC(msg); \
        } \
    } while(0)

#define MAX_BUS 2
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
} PCI_DEVICE;
#pragma pack(pop)

PCI_DEVICE *g_pci_bus[MAX_BUS][MAX_DEVICE][MAX_FUNCTION];

PCI_DEVICE* CreatePciDevice(const char *name){
    PCI_DEVICE *device = malloc(sizeof(PCI_DEVICE));
    if(!device){
        LOG_ERROR_FMT("Failed to initialize PCI_DEVICE %s", name);
        return NULL;
    }
    for(int byte = 0; byte < 256; byte++)
        device->config.data[byte] = 0;

    return device;
}

//Random ID generator (not part of real world)
uint16_t GenerateRandomID(const char *name){
    uint32_t hash = 5381;
    while (*name) {
        hash = ((hash << 5) + hash) + *name; // djb2
        name++;
    }
    return (uint16_t)(hash & 0xFFFF);
}

void SetupCommonHeader(PCI_DEVICE *device, uint16_t id, uint8_t type, uint8_t subc,
                       uint8_t cc, uint8_t pf){
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

void SetupType0Header(PCI_DEVICE *device, uint16_t id){
    uint8_t *data = device->config.data;
    PCI_HEADER_TYPE0 *type = (PCI_HEADER_TYPE0 *)(data + 0x10);
    
    for(int i = 0; i < 6; i++)
        type->bar[i] = 0x00;
    
    type->bar[0] = 0x1000;
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

void SetupType1Header(PCI_DEVICE *device, uint16_t mmL){
    uint8_t *data = device->config.data;
    PCI_HEADER_TYPE1 *type = (PCI_HEADER_TYPE1 *)(data + 0x10);

    type->bar[0] = 0x00000000;
    type->bar[1] = 0x00000000;
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

PCI_DEVICE* CreateFakeDevice(const char *name, uint8_t type, uint8_t subclass, uint8_t classcode,
                             uint8_t prog_if, uint16_t mmL){
    PCI_DEVICE *device = CreatePciDevice(name);
    if(!device){
        LOG_ERROR_FMT("Failed to initialize PCI_DEVICE %s", name);
        return NULL;
    }
    uint16_t id = GenerateRandomID(name);
    SetupCommonHeader(device, id, type, subclass, classcode, prog_if);
    if(type == 0){
        (void)mmL;
        SetupType0Header(device, id);
    }else
        SetupType1Header(device, mmL);
    
    return device;
}

void PciDevicePool(){
    g_pci_bus[0][0][0] = CreateFakeDevice("disk", 0, 0x06, 0x01, 0x01, 0);
    g_pci_bus[0][1][0] = CreateFakeDevice("net",  0, 0x00, 0x02, 0x00, 0);
    g_pci_bus[0][2][0] = CreateFakeDevice("bridge", 1, 0x04, 0x06, 0x00, 0);
    g_pci_bus[1][0][0] = CreateFakeDevice("gpu", 0, 0x00, 0x03, 0x00, 0);
}

uint16_t PciReadVendor(uint8_t bus, uint8_t dev, uint8_t func){
    PCI_DEVICE *device = g_pci_bus[bus][dev][func];
    if(!device){
        /* LOG_INFO("No device found"); */
        return 0xffff;
    }
    uint8_t *data = device->config.data;
    return *(uint16_t *)(data + 0x00);
}

void PciScanBus(uint8_t bus){
    for(int dev = 0; dev < MAX_DEVICE; dev++){ 
        for(int func = 0; func < MAX_FUNCTION; func++){
            uint16_t vendor = PciReadVendor(bus, dev, func);
            if(vendor == 0xffff)
                continue;
            
            LOG_INFO("PCI DEVICE FOUND");
            LOG_INFO_FMT("Bus=%d, Dev= %d, func= %d", bus, dev, func);

            PCI_DEVICE *device = g_pci_bus[bus][dev][func];
            uint8_t *data = device->config.data;
            uint16_t device_id = *(uint16_t *)(data + 0x02);
            uint8_t class_code = *(uint8_t *)(data + 0x0B);
            uint8_t sub_code = *(uint8_t *)(data + 0x0A);
            printf("Vendor: 0x%X Device: 0x%X\n", vendor, device_id);
            printf("Class: 0x%X Subclass: 0x%X\n", class_code, sub_code);
            uint8_t header = *(uint8_t *)(data + 0x0E) & 0x7F;

            if(header == 1){
                LOG_INFO("Bridge device detected");
                    
                PCI_HEADER_TYPE1 *h1 = (PCI_HEADER_TYPE1 *)(data + 0x10);
                if(h1->secondary_bus != bus && h1->secondary_bus < MAX_BUS){
                    LOG_INFO_FMT("Scanning Secondary Bus %d", h1->secondary_bus);
                    PciScanBus(h1->secondary_bus);
                }
            }
        }
    }
}

void PciEnumerate(){
            PciScanBus(0);
}

int main(){
    puts("Improving Foundation PCI Concept SESSION:");
    PciDevicePool();
    PciEnumerate();
    
    return 0;
}
