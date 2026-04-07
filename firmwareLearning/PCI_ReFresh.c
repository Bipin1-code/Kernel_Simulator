//Refresh PCI:  improve the foundation design which I learn but hit the wall

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>

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

#define MAX_BUS 2   //256 in real but for test it's 2
#define MAX_DEVICE 32
#define MAX_FUNCTION 8
#define MAX_REAL_DEVICE 8192 //256 * 32 (It depends on PCI_config space, 16MB(classic))

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
} PCI_DEVICE;
#pragma pack(pop)

PCI_DEVICE *g_pci_bus[MAX_BUS][MAX_DEVICE][MAX_FUNCTION];

//start
//this struct is only for to help initialisation
//this is not PCI Device expose part for OS or BIOS/UFEI
typedef struct{
    uint64_t size;
    uint8_t is_io;
    uint8_t is_64;
} BAR_CFG;
//end

PCI_DEVICE* CreatePciDevice(const char *name){
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

void SetupType0Header(PCI_DEVICE *device, uint16_t id, BAR_CFG *bars){
    uint8_t *data = device->config.data;
    PCI_HEADER_TYPE0 *type = (PCI_HEADER_TYPE0 *)(data + 0x10);
    
    for(int i = 0; i < 6; i++){
        if(bars[i].size == 0){
            type->bar[i] = 0;
            device->bar_size[i] = 0;
            continue;
        }
        device->bar_size[i] = bars[i].size;
        uint32_t flags = 0;
        if(bars[i].is_io){
            flags |= 0x1;
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

void SetupType1Header(PCI_DEVICE *device, uint16_t mmL, BAR_CFG *bars){
    uint8_t *data = device->config.data;
    PCI_HEADER_TYPE1 *type = (PCI_HEADER_TYPE1 *)(data + 0x10);

    for(int i = 0; i < 2; i++){
        if(bars[i].size == 0){
            type->bar[i] = 0;
            device->bar_size[i] = 0;
            continue;
        }
        device->bar_size[i] = bars[i].size;
        uint32_t flags = 0;
        if(bars[i].is_io){
            flags |= 0x1;
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

PCI_DEVICE* CreateFakeDevice(const char *name, uint8_t type, uint8_t subclass,
                             uint8_t classcode, uint8_t prog_if, uint16_t mmL, BAR_CFG *bars){
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

void PciDevicePool(){
    BAR_CFG disk_bars[6] = {
        {0x2000, 0, 0},
        {0},
    };

    BAR_CFG net_bars[6] = {
        {0x4000, 0, 1},
        {0},
    };

    BAR_CFG gpu_bars[6] = {
        {0x100000, 0, 1},
        {0},
    };

    BAR_CFG bridge_bars[2] = {
        {0x4000, 0, 0},
    };
    
    g_pci_bus[0][0][0] = CreateFakeDevice("disk", 0, 0x06, 0x01, 0x01, 0, disk_bars);
    g_pci_bus[0][1][0] = CreateFakeDevice("net",  0, 0x00, 0x02, 0x00, 0, net_bars);
    g_pci_bus[1][0][0] = CreateFakeDevice("gpu", 0, 0x00, 0x03, 0x00, 0, gpu_bars);
    g_pci_bus[0][2][0] = CreateFakeDevice("bridge", 1, 0x04, 0x06, 0x00, 0, bridge_bars);
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

//Debugging functions 
void DumpPciConfig(uint8_t *data){
    for(int i = 0; i < 256; i += 16){
        printf("%02X: ", i);

        for(int j = 0; j < 16; j++){
            printf("%02X ", data[i + j]);
        }
        printf("\n");
    }
}

void DumpPciDevice(uint8_t bus, uint8_t dev, uint8_t func){
    PCI_DEVICE *device = g_pci_bus[bus][dev][func];
    if(!device){
        LOG_WARN("Attempt to dump NULL device");
        return;
    }
    printf("=== PCI CONFIG DUMP ====\n");
    printf("Bus=%d Dev=%d Func=%d\n", bus, dev, func);

    DumpPciConfig(device->config.data);
}
//end

//New idea:
typedef struct _PCI_BAR_INFO{
    uint64_t base_address;
    uint64_t size;
    uint8_t is_io;
    uint8_t is_64bit;
} PCI_BAR_INFO;

typedef struct _PCI_DEVICE_CONTEXT{
    PCI_DEVICE *h_dev;
    uint8_t bus;
    uint8_t dev;
    uint8_t func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t prog_if;
    uint8_t subclass;
    uint8_t class_code;
    uint8_t header_type;

    union{
        uint32_t type0_bars[6];
        uint32_t type1_bars[2];
    } select_bar;
    
    PCI_BAR_INFO bar_info[6];    
} PCI_DEVICE_CONTEXT;

PCI_DEVICE_CONTEXT *g_pciDevCtx[MAX_REAL_DEVICE];
int g_dev_count = 0;

uint32_t PciConfigRead32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset){
    PCI_DEVICE *device = g_pci_bus[bus][dev][func];
    if(!device) return 0xffffffff;
    return *(uint32_t *)(device->config.data + offset);
}

uint32_t PciConfigWrite32(uint8_t bus, uint8_t dev, uint8_t func, uint8_t offset, uint32_t value){
    PCI_DEVICE *device = g_pci_bus[bus][dev][func];
    if(!device) return 0xffffffff;
    if(offset >= 0x10 && offset < 0x28){
        /* Mapping config-space offset -> BAR index */
        int bar_index = (offset - 0x10) / 4;
        uint32_t orig = *(uint32_t *)(device->config.data + offset);
        uint32_t type = (orig >> 1) & 0x3;
        int is_64 = (type == 2);
        if(value ==  0xffffffff){
            uint32_t size = device->bar_size[bar_index];
            if(size == 0){
                *(uint32_t *)(device->config.data + offset) = 0;
                return 0;
            }
            if(orig & 1){
                //Type = IO BAR 
                *(uint32_t *)(device->config.data + offset) =
                    ~(size - 1) | 0x1;
            }else{
                //Memory BAR
                uint64_t size64 = device->bar_size[bar_index];
                uint64_t mask64 = ~(size64 - 1);

                uint32_t low  = (uint32_t)(mask64 & 0xFFFFFFFF);
                uint32_t high = (uint32_t)(mask64 >> 32);

                uint32_t orig_low = orig;
                int is_low = ((offset - 0x10) % 8 == 0);

                if(is_64){
                    if(is_low){
                        *(uint32_t *)(device->config.data + offset) =
                            (low & ~0xf) | (orig_low & 0xf);
                    }else{
                        *(uint32_t *)(device->config.data + offset) = high;
                    }
                }else{
                    *(uint32_t *)(device->config.data + offset) =
                        (low & ~0xf) | (orig_low & 0xf);
                }
            }
            return 0;
        }
    }
    *(uint32_t *)(device->config.data + offset) = value;
    return 0;
}

PCI_BAR_INFO GetPciBarInfo(uint8_t bus, uint8_t dev, uint8_t func, uint32_t bar){
    PCI_BAR_INFO pbi = {0};

    uint8_t offset = 0x10 + bar * 4;
    //original 
    uint32_t orig_low = PciConfigRead32(bus, dev, func, offset);

    uint32_t orig_high = 0;
    uint32_t type = (orig_low >> 1) & 0x3;
    uint8_t is_64 = (type == 2);
    if(is_64){
        orig_high = PciConfigRead32(bus, dev, func, offset + 4);
    }
    
    PciConfigWrite32(bus, dev, func, offset, 0xffffffff);
    uint32_t size_low = PciConfigRead32(bus, dev, func, offset);
    if(size_low == 0){
        return pbi; //this is unsed bar so pbi is {0} for all field,
    }
    uint32_t size_high = 0;
    if(is_64){
        PciConfigWrite32(bus, dev, func, offset + 4, 0xffffffff);
        size_high = PciConfigRead32(bus, dev, func, offset + 4);
    }
    printf("DEBUG:(Response) size_low=0x%X size_high=0x%X\n", size_low, size_high);

    //Restore
    PciConfigWrite32(bus, dev, func, offset, orig_low);
    if(is_64){
        PciConfigWrite32(bus, dev, func, offset + 4, orig_high);
    }

    //Finall get all info about bar and them
    if(orig_low & 1){
        pbi.size = ~(size_low & ~0x3) + 1;
        pbi.base_address = (orig_low & ~0x3);
        pbi.is_io = 1;
    }else{
        pbi.is_64bit = is_64;
        uint64_t base = (orig_low & ~0xf);
        uint64_t size = ~(size_low & ~0xf) + 1;
        if(is_64){
            if(orig_high == 0 && size_high == 0){
                // Treat as broken/fake -> fallback to 32-bit
                pbi.base_address = (orig_low & ~0xf);
                pbi.size = ~(size_low & ~0xf) + 1;
            }else{
                base |= ((uint64_t)orig_high << 32);
                uint64_t mask = ((uint64_t)(size_low & ~0xf) |
                                 ((uint64_t)size_high << 32));
                pbi.base_address = base;
                pbi.size = ~mask + 1;
            }
        }
        else{
            pbi.base_address = base;
            pbi.size = size;
        }   
    }
    return pbi;
}

PCI_DEVICE_CONTEXT* OsCreateDevice(const uint8_t bus, const uint8_t dev, const  uint8_t func){
    PCI_DEVICE_CONTEXT *dev_ctx = malloc(sizeof(PCI_DEVICE_CONTEXT));
    if(!dev_ctx){
        LOG_ERROR("Failed to allocate memory for PCI_DEVICE_CONTEXT");
        return NULL; 
    }
    
    dev_ctx->h_dev = g_pci_bus[bus][dev][func];
    uint8_t *data = dev_ctx->h_dev->config.data;
    dev_ctx->bus = bus;
    dev_ctx->dev = dev;
    dev_ctx->func = func;
    dev_ctx->vendor_id = *(uint16_t *)(data + 0x00);
    dev_ctx->device_id = *(uint16_t *)(data + 0x02); 
    dev_ctx->prog_if = *(uint8_t *)(data + 0x09);
    dev_ctx->subclass = *(uint8_t *)(data + 0x0A);
    dev_ctx->class_code = *(uint8_t *)(data + 0x0B);
    dev_ctx->header_type = *(uint8_t *)(data + 0x0E) & 0x7F;
    
    uint32_t *bars = (uint32_t *)(data + 0x10);
    if(dev_ctx->header_type == 1){
        for(int b = 0; b < 2; b++){
            dev_ctx->select_bar.type1_bars[b] = bars[b];
            dev_ctx->bar_info[b] = GetPciBarInfo(bus, dev, func, b);
            if(dev_ctx->bar_info[b].is_64bit){
                b++;
            }
        }
    }else{
        for(int b = 0; b < 6; b++){
            dev_ctx->select_bar.type0_bars[b] = bars[b];
            dev_ctx->bar_info[b] = GetPciBarInfo(bus, dev, func, b);
            if(dev_ctx->bar_info[b].is_64bit){
                b++;
            }
        }
    }
    
    //Debug print
    int limit;
    if(dev_ctx->header_type == 1)
        limit = 2;
    else
        limit = 6;
    
    for(int b = 0; b < limit;){
        PCI_BAR_INFO *bi = &dev_ctx->bar_info[b]; 
        if(bi->size == 0){
            b++;
            continue;   
        }
        printf("BAR%d:\n", b);
        if(bi->is_64bit){
            printf(" (64-bit)\n");
        }
        printf(" Type: %s\n", bi->is_io ? "IO" : "Memory");
        printf(" Address: 0x%" PRIx64 "\n", bi->base_address);
        printf(" Size: %"  PRIu64 "\n", bi->size);

        b += (bi->is_64bit) ? 2 : 1;
    }
    
    printf("Vendor: 0x%X Device: 0x%X\n", dev_ctx->vendor_id, dev_ctx->device_id);
    printf("Class: 0x%X Subclass: 0x%X\n", dev_ctx->class_code, dev_ctx->subclass);

    return dev_ctx;
}

void PciScanBus(uint8_t bus){
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

            DumpPciDevice(bus, dev, func);

            PCI_DEVICE_CONTEXT *ctx = OsCreateDevice(bus, dev, func);
            if(ctx){
                KASSERT(g_dev_count < MAX_REAL_DEVICE, "INVALID count of device");
                g_pciDevCtx[g_dev_count++] = ctx;
            }
            
            PCI_DEVICE *device = g_pci_bus[bus][dev][func];
            uint8_t *data = device->config.data;
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
