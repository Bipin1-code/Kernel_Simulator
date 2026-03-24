
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#define MAX_CPU 4
#define MAX_DEVICES 8
#define MAX_IRQ 24
#define MMIO_SIZE 1024
#define MAX_VECTOR 256
#define LAPIC_BASE 0xFEE00000
#define MAX_VECTOR_PRIORITY 64
#define DEBURIJN64 0x03F79D71B4CB0A89
#define HANDLED 1
#define NOT_HANDLED 0

typedef struct _CPU CPU;
typedef struct _LAPIC LAPIC;
typedef struct _IOAPIC IOAPIC;
typedef struct _DEVICE DEVICE;
    
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

//these are vector priority bitmap for to handle pending vector by lapic when more interrupt came at once
static uint64_t g_vector_priority_Table[MAX_VECTOR_PRIORITY];

void BuildDeBurijnTable(){
    for(int v = 0; v < MAX_VECTOR_PRIORITY; v++){
        uint64_t mV = (1ULL << v);
        uint64_t idx = (mV * DEBURIJN64) >> 58;
        g_vector_priority_Table[idx] = v;
    }
}

uint64_t FindLowestSetBit(uint64_t bitmap){
    //just for ensurement
    if(g_vector_priority_Table[0] != 0 || g_vector_priority_Table[1] != 1) {
        BuildDeBurijnTable();  // Initialize if not done
    }
    if(bitmap == 0) return -1;
    uint64_t lowest = bitmap & -bitmap;
    uint64_t idx = (lowest * DEBURIJN64) >> 58;
    return g_vector_priority_Table[idx];
}
//It present in cpu core; means each cpu core have LAPIC
typedef struct _LAPIC{
    int id;
    uint64_t pending_vectors;
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

//NOT in use yet
//MMIO = Memory Mapped Input/Output 
unsigned char g_MMIO[MMIO_SIZE];

void DMA_Write(void *dst, void *src, size_t size){
    memcpy(dst, src, size);
}

typedef int (*fHandler)(void*);

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
    lapic->pending_vectors = 0;
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
    cpu->lapic->pending_vectors |= (1ULL << vector) ;
}

void DeviceRaiseInterrupt(DEVICE *device){
    if(!device) return;
    if(device->use_msi){
        int vector = device->msi.data;
        if(vector >= 64){
            printf("[Device: %s] Vector %d too large for bitmap\n",
                   device->name, vector);
            return;
        }
        //MSI direct vector
        printf("[Device: %s] MSI interrupt vector %d\n",
               device->name, device->msi.data);
        //Direct to LAPIC
        //for cpu id
        int cpu_id = (device->msi.address - LAPIC_BASE) >> 12;
        LAPIC *lapic = g_sys.cpus[cpu_id]->lapic;
        //device->msi.data is vector
        lapic->pending_vectors |= (1ULL << vector);
    }else{
        printf("[Device: %s] IRQ line %d\n",
               device->name, device->irq.irq_line);
        IoApicHandleIRQ(g_sys.ioapic, device->irq.irq_line);
    }
}

typedef int (*fIsrHandlerTable)(void*);

typedef struct _ISR_NODE{
    fIsrHandlerTable fhandler;
    struct _ISR_NODE *next;
} ISR_NODE;

ISR_NODE *g_isr_table[MAX_VECTOR];

void RegisterIsr(int vector, fIsrHandlerTable handler){
    ISR_NODE *node = malloc(sizeof(ISR_NODE));
    node->fhandler = handler;
    node->next = g_isr_table[vector];
    g_isr_table[vector] = node;
}
    
int LapicGetNextVector(LAPIC *lapic){
    if(lapic->pending_vectors == 0) return -1;
    int vector = FindLowestSetBit(lapic->pending_vectors);
    lapic->pending_vectors &= ~(1ULL << vector);
    lapic->in_service_vector = vector;
    return vector; 
}

void RegisterInterrupt(int vector, fHandler handler){
    g_idt[vector].handler = handler;
}

void DispatchInterrupt(CPU *cpu, int vector){
    if(vector < 32){
        printf("[CPU %d] Exception vector %d\n",
               cpu->id, vector);
        cpu->lapic->in_service_vector = -1;
        return;
    }
    ISR_NODE *node = g_isr_table[vector];
    if(!node){
        printf("[CPU %d] Unhandled interrupt vector %d\n",
               cpu->id, vector);
        return;
    }
    printf("[CPU %d] Dispatch vector %d\n",
           cpu->id, vector);
    while(node){
        if(node->fhandler(NULL) == HANDLED) break;
        node = node->next;
    }
    cpu->lapic->in_service_vector = -1;
}

void CpuStep(CPU *cpu){
    int vector = LapicGetNextVector(cpu->lapic);
    if(vector != -1){
        DispatchInterrupt(cpu, vector);
    }
}

int KeyboardHandler(void *ctx){
    (void)ctx;
    printf("[ISR] Keyboard interrupt handled\n");
    return HANDLED;
}

//test functions:
int FakeHandler(void *ctx){
    (void)ctx;
    printf("[ISR] Not mine\n");
    return NOT_HANDLED;
}

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
    printf("[LAPIC: %d] pending mask = %llu, in-service = %d\n",
           lapic->id,
           lapic->pending_vectors,
           lapic->in_service_vector);
}

void SystemInit(){
    BuildDeBurijnTable();
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

    puts("System Running>>>");
    
    RegisterInterrupt(33, KeyboardHandler);
    RegisterIsr(33, KeyboardHandler);
    RegisterIsr(33, FakeHandler);
    
    
    DeviceRaiseInterrupt(g_sys.devices[0]);
    DumpLAPIC(g_sys.cpus[0]->lapic);
    CpuStep(g_sys.cpus[0]);

    puts("System Terminated>>>");
}

int main(){
    puts("Inpterrupt Understand Gaining Phase:");
    SystemInit();
    return 0;
}
