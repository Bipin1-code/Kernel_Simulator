
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <assert.h>

#define LOG_INFO(fmt, ...) \
    printf("\x1b[32m[INFO]\x1b[0m " fmt "\n", ##__VA_ARGS__)

#define LOG_WARN(fmt, ...)\
    printf("\x1b[33m[WARN]\x1b[0m " fmt "\n", ##__VA_ARGS__)

#define LOG_ERROR(fmt, ...)\
    printf("\x1b[31m [ERROR]\x1b[0m " fmt "\n", ##__VA_ARGS__)

#define PANIC(fmt, ...)\
    do{ \
        printf("\n\x1b[31m[PANIC]\x1b[0m " fmt "\n", ##__VA_ARGS__); \
        printf("File: %s Line: %d\n", __FILE__, __LINE__); \
        exit(1); \
    } while(0)

#define KASSERT(cond, fmt, ...) \
    do{ \
        if(!(cond)){ \
            PANIC(fmt, ##__VA_ARGS__); \
            } \
    } while(0)


#define MAX_DEVICES 32
#define REG_STATUS  0x0
#define REG_CONTROL 0x4
#define REG_DATA    0x8

#define PCI_BAR_IO 0x1
#define PCI_BAR_MEM_TYPE_64 0x4
#define PCI_BAR_MEM_MASK (~0xf)
#define PCI_BAR_IO_MASK (~0x3)

#define IO_SPACE_SIZE 65536

typedef struct _PCI_CONFIG_SPACE{
    uint16_t vendor_id;
    uint16_t device_id;
    uint16_t command; //what OS tells the device to do
    uint16_t status; //what device tells the OS about itself

    uint8_t revision_id;
    uint8_t prog_if; //how exactly a device behaves within a subclass
    uint8_t subclass;
    uint8_t class_code;

    uint8_t cache_line_size;
    uint8_t latency_timer;
    uint8_t header_type;
    uint8_t bist;

    //Each index contains Base address register
    uint64_t bar[6];

    //Subsystem Info
    uint16_t subsystem_vendor_id;
    uint16_t subsystem_id;
} PCI_CONFIG_SPACE;

typedef struct _FAKE_PCI_DEVICE{
    PCI_CONFIG_SPACE *config;
    uint32_t bar_size[6]; //size of each bar
    void *device_memory[6]; //actual backing memory
    int bar_probe[6]; //not part of real PCI device
} FAKE_PCI_DEVICE;

FAKE_PCI_DEVICE *g_pci_devices[MAX_DEVICES];
int g_device_count = 0;

uint8_t g_io_space[IO_SPACE_SIZE];

uint32_t g_io_base = 0x1000;

uint32_t AllocateIoPort(uint32_t size){
    uint32_t port = g_io_base;
    g_io_base += size;
    KASSERT(g_io_base < IO_SPACE_SIZE, "IO space exhausted");
    return port;
}

uint32_t AllocatePhysical(uint32_t size){
    static uint32_t phys_base = 0x10000000;
    uint32_t addr = phys_base;
    phys_base += size;
    return addr;
}

void* MapToVirtual(uint64_t phys_addr, uint32_t size){
    (void)phys_addr;
    return malloc(size);
}

PCI_CONFIG_SPACE* CreatePciConfigSpace(const char *name, const uint16_t v_id, const uint8_t cc, const uint8_t sc){
    PCI_CONFIG_SPACE *pcs = malloc(sizeof(PCI_CONFIG_SPACE));
    if(!pcs){
        LOG_WARN("Failed to Create PCI_CONFIG_SPACE for device: %s",
               name);
        return NULL;
    }
    
    pcs->vendor_id = v_id;
    pcs->device_id = (v_id * 13) & 0xffff;
    pcs->command = 0x0000;
    pcs->status = 0x0010; 
    pcs->revision_id = 0x01;
    pcs->prog_if = 0x00;
    pcs->subclass = sc;
    pcs->class_code = cc;
    pcs->cache_line_size = 16;
    pcs->latency_timer = 0x00;
    pcs->header_type = 0x00;
    pcs->bist = 0x00;

    for(int i = 0; i < 6; i++){
        pcs->bar[i] = 0;
    }
    pcs->subsystem_vendor_id = v_id;
    pcs->subsystem_id = 0x0001;
   
    return pcs;
}

FAKE_PCI_DEVICE* InitFakePCIDevice(const char *name, const uint16_t v_id, const uint8_t cc, const uint8_t sc){
    FAKE_PCI_DEVICE *fpd = malloc(sizeof(FAKE_PCI_DEVICE));
    if(!fpd){
        LOG_WARN("Failed to initialize PCI Device %s", name);
        return NULL;
    }
    fpd->config = CreatePciConfigSpace(name, v_id, cc, sc);
    if(!fpd->config){
        free(fpd);
        LOG_WARN("Failed to Create PCI_CONFIG_SPACE for device: %s\n",
               name);
        return NULL;
    }

    for(int i = 0; i < 6; i++){
        fpd->bar_size[i] = 0;
        fpd->device_memory[i] = NULL;
    }
    
    fpd->bar_size[0] = 256;
    fpd->config->bar[0] = 0x0;
    
    return fpd;
}

uint32_t IoRead(uint32_t port, uint32_t offset){
    uint32_t addr = port + offset;
    KASSERT((addr + 4) <= IO_SPACE_SIZE, "IO read out of range");
    return *(uint32_t *)&g_io_space[addr];
}

void IoWrite(uint32_t port, uint32_t offset, uint32_t value){
    uint32_t addr = port + offset;
    KASSERT((addr + 4) <= IO_SPACE_SIZE, "IO write out of range");
    *(uint32_t *)&g_io_space[addr] = value;
}

void* GetBarMemory(FAKE_PCI_DEVICE *device, int bar_index){
    KASSERT(device != NULL, "Device is NULL");
    KASSERT(device->config != NULL, "Config space missing");
    
    if(bar_index < 0 || bar_index >= 6){
        LOG_ERROR("Invalid BAR index");
        return NULL;
    }
    if(!(device->config->command & (1 << 1))){
        LOG_WARN("Memory access denied: device not enabled");
        return NULL;
    }

    uint32_t bar_val = (uint32_t)device->config->bar[bar_index];

    uint32_t addr;
    if(bar_val & PCI_BAR_IO){
        addr = bar_val & PCI_BAR_IO_MASK;
    } else {
        addr = bar_val & PCI_BAR_MEM_MASK;
    }
    (void)addr;
    
    if(device->config->bar[bar_index] == 0){
        LOG_ERROR("BAR %d not mapped", bar_index);
        return NULL;
    }
    return device->device_memory[bar_index];
}

void PciWriteBar(FAKE_PCI_DEVICE *device, int bar_index, uint32_t value){
    if(value == 0xffffffff){
        device->bar_probe[bar_index] = 1;
    }else{
        device->config->bar[bar_index] = value;
    }
}

uint32_t PciReadBar(FAKE_PCI_DEVICE *device, int bar_index){
    if(device->bar_probe[bar_index]){
        device->bar_probe[bar_index] = 0;
        uint32_t size = device->bar_size[bar_index];
        return ~(size - 1);
    }
    uint32_t bar_val = (uint32_t)device->config->bar[bar_index];
    if(bar_val & PCI_BAR_IO){
        return bar_val & PCI_BAR_IO_MASK;
    } else {
        return bar_val & PCI_BAR_MEM_MASK;
    };   
}

uint32_t ProbeBarSize(FAKE_PCI_DEVICE *device, int bar_index){
    uint32_t raw_bar =(uint32_t)device->config->bar[bar_index];
    int is_64 = (!(raw_bar & PCI_BAR_IO)) && (raw_bar & PCI_BAR_MEM_TYPE_64);
    
    uint32_t original_low = device->config->bar[bar_index];
    uint32_t original_high = 0;
    if(is_64)
        original_high = device->config->bar[bar_index + 1];
    
    PciWriteBar(device, bar_index, 0xffffffff);
    if(is_64)
        PciWriteBar(device, bar_index + 1, 0xffffffff);

    uint32_t low_value = PciReadBar(device, bar_index);
    uint64_t value = low_value;
    if(is_64){
        uint32_t high_value = PciReadBar(device, bar_index + 1);
        value |= ((uint64_t)high_value << 32);
    }
    
    uint64_t size;
    if(raw_bar & PCI_BAR_IO)
        size = ~(value & ~0x3ULL) + 1;
    else
        size = ~(value & ~0xFULL) + 1;
    
    PciWriteBar(device, bar_index, original_low);
    if(is_64)
        PciWriteBar(device, bar_index + 1, original_high);

    return (uint32_t)size;
}

void DecodeBar(uint32_t bar_value, int index){
    if(bar_value & PCI_BAR_IO){
        LOG_INFO("BAR%d Type: IO Space", index);
    }
    else{
        LOG_INFO("BAR%d Type: MMIO", index);
        if(bar_value &  PCI_BAR_MEM_TYPE_64)
            LOG_INFO("BAR%d is 64-bit", index);
        else
            LOG_INFO("BAR%d is 32-bit", index);
    }
}

void InitDeviceRegisters(FAKE_PCI_DEVICE *device){
    KASSERT(device != NULL, "Device is NULL");
    uint32_t bar_val = (uint32_t)device->config->bar[0];
    if(bar_val & PCI_BAR_IO){
        uint32_t port = bar_val & PCI_BAR_IO_MASK;
        IoWrite(port, REG_STATUS, 0);
        IoWrite(port, REG_CONTROL, 0);
        IoWrite(port, REG_DATA, 0);
        LOG_INFO("IO Device registers initialized");
    }
    else{
        KASSERT(device->device_memory[0] != NULL, "Device memory not mapped!");
        uint8_t *mem = device->device_memory[0];
        *(uint32_t *)(mem + REG_STATUS) = 0;
        *(uint32_t *)(mem + REG_CONTROL) = 0;
        *(uint32_t *)(mem + REG_DATA) = 0;
        
        LOG_INFO("MMIO Device registers initialized");
    }
}

void RegisterDevice(FAKE_PCI_DEVICE *device){
    if(g_device_count < MAX_DEVICES){
        g_pci_devices[g_device_count++] = device;
        return;
    }    
    LOG_WARN("System cannot support more than %d devices", MAX_DEVICES);
}

void DevicePool(){
    RegisterDevice(InitFakePCIDevice("Keyboard", 0x3431, 0x09, 0x00));
    RegisterDevice(InitFakePCIDevice("Display", 0x4231, 0x03, 0x00));
    RegisterDevice(InitFakePCIDevice("Speaker", 0x1425, 0x04, 0x01));
    FAKE_PCI_DEVICE *mic = InitFakePCIDevice("Microphone", 0x3371, 0x04, 0x01);
    mic->config->bar[0] = PCI_BAR_IO;
    mic->bar_size[0] = 128;
    RegisterDevice(mic);
}

//Not in used
uint64_t GetBarAddress64(FAKE_PCI_DEVICE *device, int bar){
    uint32_t low = (uint32_t)device->config->bar[bar];
    uint32_t high = (uint32_t)device->config->bar[bar + 1];
    low &= PCI_BAR_MEM_MASK;
    return ((uint64_t)high << 32) | low;
}

void PciEnumerate(){
    puts("\n[PCI ENUMERATION START]\n");
    for(int i = 0; i < g_device_count; i++){
        FAKE_PCI_DEVICE *device = g_pci_devices[i];
        
        if(device->config->vendor_id == 0xffff)
            continue;
        
        LOG_INFO("\n Device Found:");
        LOG_INFO(" Vendor ID: 0x%X", device->config->vendor_id);
        LOG_INFO(" Device ID: 0x%X", device->config->device_id);
        LOG_INFO(" Class: 0x%X", device->config->class_code);
        LOG_INFO(" Sub Class: 0x%X", device->config->subclass);

        for(int bar = 0; bar < 6; bar++){
            if(device->bar_size[bar] == 0)
                continue;

            uint32_t raw_bar = device->config->bar[bar];
            DecodeBar(raw_bar, bar);
            
            LOG_INFO("Probing BAR%d...", bar);
            uint32_t size = ProbeBarSize(device, bar);
            KASSERT(size > 0, "Invalid BAR size!");
            LOG_INFO("BAR%d size: %u bytes", bar, size);

            //Preserve type bits when writing BAR
            if(raw_bar & PCI_BAR_IO){
                uint32_t addr = AllocateIoPort(size);
                LOG_INFO("BAR%d assigned IO PORT: 0x%X", bar, addr);
                PciWriteBar(device, bar, addr | PCI_BAR_IO);
            }
            else if(raw_bar & PCI_BAR_MEM_TYPE_64){
                uint32_t flags = raw_bar & 0xf;
                uint64_t full_addr = (uint64_t)AllocatePhysical(size);
                uint32_t low = (uint32_t)(full_addr & 0xffffffff);
                uint32_t high = (uint32_t)(full_addr >> 32);
                LOG_INFO("BAR%d 64-bit addr: 0x%llX", bar, full_addr);
                PciWriteBar(device, bar, low | flags);
                PciWriteBar(device, bar + 1, high);
                void *virt = MapToVirtual(full_addr, size);
                device->device_memory[bar] = virt;
                LOG_INFO("BAR%d mapped VIRTUAL: %p", bar, virt);
                LOG_INFO("BAR%d consumes BAR%d as well as (64-bit)", bar , bar+ 1);
                bar++;
            }
            else{
                uint32_t flags = raw_bar & 0xf;
                uint32_t addr = AllocatePhysical(size);
                LOG_INFO("BAR%d assigned PHYSICAL: 0x%X", bar, addr);
                PciWriteBar(device, bar, addr | flags);
                void *virt = MapToVirtual(addr, size);
                device->device_memory[bar] = virt;
                LOG_INFO("BAR%d mapped VIRTUAL: %p", bar, virt);
            }
        }  
        device->config->command |=  (1 << 0) | (1 << 1);
        InitDeviceRegisters(device);
    }
}

uint32_t ReadDeviceReg(FAKE_PCI_DEVICE *device, int bar, uint32_t offset){
    uint32_t bar_val = (uint32_t)device->config->bar[bar];
    if(bar_val & PCI_BAR_IO){
        uint32_t port = bar_val & PCI_BAR_IO_MASK;
        return IoRead(port, offset);
    }
    else{
        uint8_t *base = (uint8_t *)GetBarMemory(device, bar);
        if(!base){
            LOG_WARN("BAR %d is not mapped to virtual memory", bar);
            return 0;
        }
        return *(uint32_t *)(base + offset);
    }
}

void WriteDeviceReg(FAKE_PCI_DEVICE *device, int bar, uint32_t offset, uint32_t value){
    uint32_t bar_val = (uint32_t)device->config->bar[bar];
    if(bar_val & PCI_BAR_IO){
        uint32_t port = bar_val & PCI_BAR_IO_MASK;
        IoWrite(port, offset, value);
        if(offset == REG_CONTROL && value == 1){
            LOG_INFO("IO Device START command");
            IoWrite(port, REG_STATUS, 1);
        }
        if(offset == REG_DATA){
            LOG_INFO("IO Device received DATA: %u", value);
        }
    }
    else{
        uint8_t *base = (uint8_t *)GetBarMemory(device, bar);
        if(!base){
            LOG_WARN("BAR %d MMIO not mapped", bar);
            return;
        }
        switch(offset){
            case REG_CONTROL:{
                *(uint32_t *)(base + offset) = value;
                if(value == 1){
                    LOG_INFO("MMIO Device START command received");
                    *(uint32_t *)(base + REG_STATUS) = 1;
                }
                break;
            }
            case REG_DATA:{
                *(uint32_t *)(base + offset) = value;
                LOG_INFO("MMIO Device received DATA: %u", value);
                break;
            }
            default:
            *(uint32_t*)(base + offset) = value;   
        }
    }
}

void TestDevice(FAKE_PCI_DEVICE *dev){
    dev->config->command |= (1 << 1); // enable memory

    WriteDeviceReg(dev, 0, REG_CONTROL, 1);

    uint32_t status = ReadDeviceReg(dev, 0, REG_STATUS);

    LOG_INFO("Device status: %u", status);

    WriteDeviceReg(dev, 0, REG_DATA, 1234);
}

void AttachDriver(FAKE_PCI_DEVICE *device){
    if(device->config->class_code == 0x09){
        TestDevice(device);
    }
    if(device->config->class_code == 0x03){
        TestDevice(device);
    }
    if(device->config->class_code == 0x04){
        TestDevice(device);
    }
    if(device->config->class_code == 0x01){
        TestDevice(device);
    }
    if(device->config->class_code == 0x02){
        TestDevice(device);
    }
    //more...
}

int main(){
    puts("Learn about PCI");
    DevicePool();
    PciEnumerate();
    for(int i = 0; i < g_device_count; i++){
        AttachDriver(g_pci_devices[i]);
    }

    return 0;
}
