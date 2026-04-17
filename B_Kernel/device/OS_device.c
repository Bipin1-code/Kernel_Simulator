
#include "OS_device.h"
#include "log.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>

PCI_DEVICE_CONTEXT *g_pciDevCtx[MAX_REAL_DEVICE];
int g_dev_count = 0;

static uint64_t gNextMimoAddr = 0xf0000000; //start of MMIO region
static uint64_t gNextIoAddr = 0x1000;  //start of IO port region

static uint64_t AllocateMmioAddress(uint64_t size){
    uint64_t addr = (gNextMimoAddr + size - 1) & ~(size - 1);
    gNextMimoAddr = addr + size;
    return addr;
}

static uint64_t AllocateIoAddress(uint64_t size){
    uint64_t addr = gNextIoAddr;
    gNextIoAddr += size;
    return addr;
}

static void AssignBarAddress(PCI_DEVICE_CONTEXT *ctx){
    for(int i = 0; i < 6; i++){
        if(ctx->bar_info[i].size == 0) continue;

        uint64_t addr;
        if(ctx->bar_info[i].is_io){
            addr = AllocateIoAddress(ctx->bar_info->size);
        }else{
            addr = AllocateMmioAddress(ctx->bar_info->size);
        }

        PciSetBarAddress(ctx->bus, ctx->dev, ctx->func, i, addr);
        ctx->bar_info[i].base_address = addr;

        LOG_INFO_FMT("Assigned BAR%d address: 0x%" PRIx64, i, addr);
    }
}


static void (*fgNewDeviceNotifier)(PCI_DEVICE_CONTEXT *device) = NULL;

void OS_RegisterDeviceFoundCallback(void (*callback)(PCI_DEVICE_CONTEXT *)){
    fgNewDeviceNotifier = callback;
}

static void* DeviceGetBarMemory(PCI_DEVICE_CONTEXT *ctx, int bar_index){
    if(!ctx || bar_index < 0 || bar_index >= 6)
        return NULL;
    
    return PciGetBarMemory(ctx->bus, ctx->dev, ctx->func, bar_index);
}

static uint64_t DeviceBarRead(PCI_DEVICE_CONTEXT *ctx, int bar, uint64_t offset,
                              int size){
    if(!ctx) return 0;
    return PciReadBar(ctx->bus, ctx->dev, ctx->func, bar, offset, size);
}

static void DeviceBarWrite(PCI_DEVICE_CONTEXT *ctx, int bar, uint64_t offset,
                           uint64_t value, int size){
    PciWriteBar(ctx->bus, ctx->dev, ctx->func, bar, offset, value, size);
}

PCI_DEVICE_CONTEXT* DeviceCreateFromPci(const uint8_t bus, const uint8_t dev,
                                        const uint8_t func){

    PCI_DEVICE_CONTEXT *dev_ctx = calloc(1, sizeof(PCI_DEVICE_CONTEXT));
    if(!dev_ctx){
        LOG_ERROR("Failed to allocate memory for PCI_DEVICE_CONTEXT");
        return NULL; 
    }
    
    uint32_t reg0 = PciConfigRead32(bus, dev, func, 0x00);
    dev_ctx->bus = bus;
    dev_ctx->dev = dev;
    dev_ctx->func = func;
    dev_ctx->vendor_id = reg0 & 0xffff;
    dev_ctx->device_id = (reg0 >> 16) & 0xffff;
    //uint32_t reg1 = PciConfigRead32(bus, dev, func, 0x04); //for command and status on register 1
    uint32_t reg2 = PciConfigRead32(bus, dev, func, 0x08);
    dev_ctx->revision_id = reg2 & 0xff;
    dev_ctx->prog_if = (reg2 >> 8) & 0xff;
    dev_ctx->subclass = (reg2 >> 16) & 0xff;
    dev_ctx->class_code = (reg2 >> 24) & 0xff;
    uint32_t reg3 = PciConfigRead32(bus, dev, func, 0x0c);
    dev_ctx->header_type = (reg3 >> 16) & 0x7F;

    int limit = (dev_ctx->header_type == 1) ? 2 : 6;
        
    for(int b = 0; b < limit;){
        PCI_BAR_INFO bi = GetPciBarInfo(bus, dev, func, b);
        dev_ctx->bar_info[b] = bi;
        uint32_t raw = PciConfigRead32(bus, dev, func, (0x10 + b * 4));
                
        if(dev_ctx->header_type == 1){
            if(b < 2 )
                dev_ctx->select_bar.type1_bars[b] = raw;
        }else{
            if(b < 6)
                dev_ctx->select_bar.type0_bars[b] = raw;
        }
            
        if(bi.size == 0){
            b++;
            continue;
        }

        AssignBarAddress(dev_ctx);
        
        //Debug  start
        printf("BAR%d\n", b);
        if(bi.is_64bit) printf(" (64-bit)\n");
        printf(" Type: %s\n", bi.is_io ? "IO" : "Memory");
        printf("Address: 0x%" PRIx64 "\n", dev_ctx->bar_info[b].base_address);
        printf(" Address: 0x%" PRIx64 "\n", bi.base_address); //this make sure host is assigning the BAR as expected
        printf(" Size: 0x%" PRIx64 " (%" PRIu64 " bytes)\n", bi.size, bi.size);
        //Debug end
        
        b += (bi.is_64bit) ? 2 : 1;
    }


    //These are also debug print lines
    printf("Vendor: 0x%04X Device: 0x%04X\n", dev_ctx->vendor_id, dev_ctx->device_id);
    printf("Class: 0x%02X Subclass: 0x%02X\n", dev_ctx->class_code, dev_ctx->subclass);
    printf("Revision_ID: 0x%02X\n", dev_ctx->revision_id);
    printf("Header Type: 0x%02X\n", dev_ctx->header_type);

    //new field iniliatize
    dev_ctx->driver = NULL;
    dev_ctx->device_data = NULL;

    dev_ctx->fBarGetMem = DeviceGetBarMemory;
    dev_ctx->fBarRead = DeviceBarRead;
    dev_ctx->fBarWrite = DeviceBarWrite;
    
    return dev_ctx;
}

void OS_HandleNewPciDevice(const uint8_t bus, const uint8_t dev, const uint8_t func){
     PCI_DEVICE_CONTEXT *ctx = DeviceCreateFromPci(bus, dev, func);
     if(ctx){
         KASSERT(g_dev_count < MAX_REAL_DEVICE, "INVALID count of device");
         g_pciDevCtx[g_dev_count++] = ctx;
     
         if(fgNewDeviceNotifier){
             fgNewDeviceNotifier(ctx);
         }
     }
}
