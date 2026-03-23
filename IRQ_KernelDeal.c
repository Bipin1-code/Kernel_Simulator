
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct _CPU CPU;
typedef struct _LAPIC LAPIC;
typedef struct _IOAPIC IOAPIC;
typedef struct _DEVICE DEVICE;

#define MAX_CPU 4
#define MAX_DEVICES 8
#define MAX_IRQ 24
#define MMIO_SIZE 1024
#define MAX_VECTOR 256
#define LAPIC_BASE 0xFEE00000

typedef struct _SYSTEM{
    CPU *cpus[MAX_CPU];
    int c_count;
    IOAPIC *ioapic;
    DEVICE *devices[MAX_DEVICES];
    int d_count;
} SYSTEM;

SYSTEM g_sys;

typedef struct _CPU{
    int id;
    LAPIC *lapic;
    int running;
} CPU;

typedef struct _LAPIC{
    int id;
    int pending_vector;
    int in_service_vector;
    CPU *owner_cpu;
} LAPIC;

typedef struct{
    int irq;
    int vector;
    int target_cpu;
} IOAPIC_ROUTE;

typedef struct _IOAPIC{
    IOAPIC_ROUTE routes[MAX_IRQ];
} IOAPIC;

typedef struct{
    int supported; //msi support or not
    int multi_msg; //how many vectors supported
} MSI_CAP;

typedef struct{
    unsigned int address;
    unsigned int data;
    int enabled;
} MSI_CONFIG;

typedef struct{
    int irq_line;
} IRQ_CONFIG;

typedef struct _DEVICE{
    char name[32];
    const MSI_CAP *msi_cap;
    MSI_CONFIG msi;
    IRQ_CONFIG irq;
    int use_msi;
} DEVICE;

//MMIO = Memory Mapped Input/Output 
unsigned char g_MMIO[MMIO_SIZE];

void DMA_Write(void *dst, void *src, size_t size){
    memcpy(dst, src, size);
}

typedef void (*fHandler)(void*);

typedef struct{
    fHandler handler;
} IDT_ENTRY;

//Interrupt descriptor table
IDT_ENTRY g_idt[MAX_VECTOR];

//We are manually writting ids but it's not a good practice
//It learn phase so we can do that for now 
LAPIC* CreateLAPIC(int id, CPU *owner){
    LAPIC *lapic = malloc(sizeof(LAPIC));
    if(!lapic){
        puts("Memory allocation failed for LAPIC variable");
        return NULL;
    }
    lapic->id = id;
    lapic->owner_cpu = owner;
    lapic->in_service_vector = -1;
    lapic->pending_vector = -1;
    return lapic;
}

CPU* CreateCPUCore(int cpu_id){
    CPU *cpu= malloc(sizeof(CPU));
    if(!cpu){
        puts("Memory allocation failed for CPU instance");
        return NULL;
    }
    cpu->id = cpu_id;
    cpu->lapic = CreateLAPIC(cpu_id, cpu);
    cpu->running = 0;
    return cpu;
}

CPU* RecognisedCPU(int id){
    return CreateCPUCore(id);
}

DEVICE* CreateDevice(const char *name, const MSI_CAP *msi_cap){
    DEVICE *dev = calloc(1, sizeof(DEVICE));
    if(!dev) return NULL;
    strncpy(dev->name, name, sizeof(dev->name) - 1);
    dev->name[sizeof(dev->name) - 1] = '\0';
    dev->msi_cap = msi_cap;
    return dev;
}

//We support one vector  for now
static const MSI_CAP msi_cap_1vec = {.supported = 1, .multi_msg = 1};
static const MSI_CAP msi_cap_none = {.supported = 0, .multi_msg = 0};

void SetupIoApicRoute(int irq, int vector, int cpu_id){
    if(!g_sys.ioapic) return;
    g_sys.ioapic->routes[irq].irq = irq;
    g_sys.ioapic->routes[irq].vector = vector;
    g_sys.ioapic->routes[irq].target_cpu = cpu_id;
}

void ConfigureDevice(DEVICE *device, CPU *cpu, int vector){
    if(!device) return;
    if(device->msi_cap->supported == 1){
        device->msi.address = LAPIC_BASE + (cpu->id << 12);
        device->msi.data = vector;
        device->msi.enabled = 1;
        device->use_msi = 1;
        //Invalidate IRQ path
        device->irq.irq_line = -1;
    }else{
        device->irq.irq_line = 1;
        device->use_msi = 0;
        //Invalidate MSI path
        device->msi.address = 0;
        device->msi.data = 0;
        device->msi.enabled = 0;
        //Set up IOAPIC
        SetupIoApicRoute(device->irq.irq_line, vector, cpu->id);
    }
}

void DevicePool(){
    DEVICE *keyboard = CreateDevice("keyboard", &msi_cap_1vec);
    ConfigureDevice(keyboard, g_sys.cpus[0], 33);
    g_sys.devices[g_sys.d_count++] = keyboard;

    DEVICE *mouse = CreateDevice("mouse", &msi_cap_1vec);
    ConfigureDevice(mouse, g_sys.cpus[1], 34);
    g_sys.devices[g_sys.d_count++] = mouse;

    DEVICE *microphone = CreateDevice("microphone", &msi_cap_none);
    ConfigureDevice(microphone, g_sys.cpus[2], 35);
    g_sys.devices[g_sys.d_count++] = microphone;
}

/*
  First Signal flow
  Device -> IOAPIC -> LAPIC (pending only)
 */
void IoApicHandleIRQ(IOAPIC *ioapic, int irq){
    if(irq < 0 || irq >= MAX_IRQ){
        printf("[IOAPIC] Invalid IRQ %d\n", irq);
        return;
    }
    IOAPIC_ROUTE *rte = &ioapic->routes[irq];
    int vector = rte->vector;
    int cpu_id = rte->target_cpu;

    printf("[IOAPIC] IRQ %d -> Vector %d -> CPU %d\n",
           irq, vector, cpu_id);
    CPU *cpu = g_sys.cpus[cpu_id];
    if(!cpu || !cpu->lapic) return;
    cpu->lapic->pending_vector = vector;
}

void DeviceRaiseInterrupt(DEVICE *device){
    if(!device) return;
    if(device->use_msi){
        //MSI direct vector
        printf("[Device: %s] MSI interrupt vector %d\n",
               device->name, device->msi.data);
        //Direct to LAPIC
        //for cpu id
        int cpu_id = (device->msi.address - LAPIC_BASE) >> 12;
        LAPIC *lapic = g_sys.cpus[cpu_id]->lapic;
        lapic->pending_vector = device->msi.data;
    }else{
        printf("[Device: %s] IRQ line %d\n",
               device->name, device->irq.irq_line);
        IoApicHandleIRQ(g_sys.ioapic, device->irq.irq_line);
    }
}

//test functions:
int IsLapic_id_n_owner(LAPIC *lapic){
    if (!lapic) {
        printf("LAPIC is NULL\n");
        return 0;
    }
    if (!lapic->owner_cpu) {
        printf("LAPIC has no owner CPU\n");
        return 0;
    }
    int check = lapic->owner_cpu->id;
    if (check < 0 || check > 4) {
        printf("Invalid CPU id: %d\n", check);
        return 0;
    }
    printf("LAPIC id = %d, Owner CPU id = %d\n",
           lapic->id,
           lapic->owner_cpu->id);
    return 1;
}

void DisplayDeviceConfig(DEVICE *device){
    if(device->use_msi)
        printf("%s -> MSI:%d addr:0x%x data:%d\n",
               device->name,
               device->use_msi,
               device->msi.address,
               device->msi.data);

    else
        printf("%s -> IRQ:%d\n",
               device->name,
               device->irq.irq_line);
}

void DumpLAPIC(LAPIC *lapic){
    printf("[LAPIC: %d] pending = %d, in-service = %d\n",
           lapic->id,
           lapic->pending_vector,
           lapic->in_service_vector);
}

void SystemInit(){
    g_sys.c_count = 0;
    g_sys.d_count = 0;
    while(g_sys.c_count < MAX_CPU){
        int passVal = g_sys.c_count;
        g_sys.cpus[g_sys.c_count++] = RecognisedCPU(passVal);
    }
    g_sys.ioapic = calloc(1, sizeof(IOAPIC));
    if(!g_sys.ioapic){
        puts("Memory allocation failed for IOAPIC instance");
        return;
    }
    
    DevicePool();
    // testing device config
    for(int i = 0; i < g_sys.d_count; i++){
        DisplayDeviceConfig(g_sys.devices[i]);
    }
    puts("System Booting>>>");
    printf("System has %d cpu core\nConnected %d devices\n",
           g_sys.c_count, g_sys.d_count);

    DeviceRaiseInterrupt(g_sys.devices[0]);
    DumpLAPIC(g_sys.cpus[0]->lapic);
    puts("System Running>>>");
}

int main(){
    puts("Inpterrupt Understand Gaining Phase:");
    SystemInit();
    return 0;
}
