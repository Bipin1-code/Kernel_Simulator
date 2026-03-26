
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
#define BUF_SIZE 16

#define LOG_HW(fmt, ...)     printf("[HW] " fmt "\n", ##__VA_ARGS__)
#define LOG_KERN(fmt, ...)   printf("[KERNEL] " fmt "\n", ##__VA_ARGS__)
#define LOG_FATAL(fmt, ...)  printf("[FATAL] " fmt "\n", ##__VA_ARGS__)

typedef struct _CPU CPU;
typedef struct _CPU_CONTEXT CPU_CONTEXT;
typedef struct _LAPIC LAPIC;
typedef struct _IOAPIC IOAPIC;
typedef struct _DEVICE DEVICE;
typedef struct _DPC DPC;
    
typedef struct _SYSTEM{
    CPU *cpus[MAX_CPU];
    CPU_CONTEXT *context[MAX_CPU];
    int c_count;
    IOAPIC *ioapic;
    DEVICE *devices[MAX_DEVICES];
    int d_count;
} SYSTEM;

SYSTEM g_sys;

typedef enum{
    PASSIVE_LVL = 0,
    DISPATCH_LVL,
    DEVICE_LVL,
    HIGH_LVL
} IRQL;

typedef struct _CPU{
    int id;
    LAPIC *lapic;
    int running;
} CPU;

typedef struct _CPU_CONTEXT{
    CPU *cpu;
    IRQL irql;
    DPC *dpc_head;
    DPC *dpc_tail;
} CPU_CONTEXT;

int RaiseIrql(CPU_CONTEXT *cpu_ctx, IRQL newIrql);
void LowerIrql(CPU_CONTEXT *cpu_ctx, IRQL oldIrql);

//these are vector priority bitmap for to handle pending vector by lapic when more interrupt came at once
static uint64_t g_vector_priority_Table[MAX_VECTOR_PRIORITY];

void BuildDeBurijnTable(){
    for(int v = 0; v < MAX_VECTOR_PRIORITY; v++){
        uint64_t mV = (1ULL << v);
        uint64_t idx = (mV * DEBURIJN64) >> 58;
        g_vector_priority_Table[idx] = v;
    }
}

int FindLowestSetBit(uint64_t bitmap){
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

typedef struct _RING_BUFFER{
    int data[BUF_SIZE];
    int head;
    int tail;
} RING_BUFFER;

typedef struct _DEVICE{
    char name[32];
    const MSI_CAP *msi_cap;
    MSI_CONFIG msi;
    IRQ_CONFIG irq;
    int use_msi;
    RING_BUFFER buffer;
} DEVICE;

int RB_IsEmpty(RING_BUFFER *rb){
    return rb->head == rb->tail;
}
int RB_IsFull(RING_BUFFER *rb){
    return ((rb->tail + 1) % BUF_SIZE) == rb->head;
}

void RB_Push(RING_BUFFER *rb, int val){
    if(RB_IsFull(rb)){
        LOG_KERN("[RB] Overflow, value %d lost\n", val);
        return;
    }
    rb->data[rb->tail] = val;
    rb->tail = (rb->tail + 1) % BUF_SIZE;
}

int RB_Pop(RING_BUFFER *rb, int *out){
    if(RB_IsEmpty(rb)) return 0;
    *out = rb->data[rb->head];
    rb->head = (rb->head + 1) % BUF_SIZE;
    return 1;
}

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
    if(!owner){
        LOG_FATAL("Cannot create LAPIC with NULL owner\n");
        return NULL;
    }
    if(id < 0 || id >= MAX_CPU){
        LOG_FATAL("Invalid LAPIC id %d\n", id);
        return NULL;
    }
    LAPIC *lapic = malloc(sizeof(LAPIC));
    if(!lapic){
        LOG_FATAL("Memory allocation failed for LAPIC variable");
        return NULL;
    }
    lapic->id = id;
    lapic->owner_cpu = owner;
    lapic->in_service_vector = -1;
    lapic->pending_vectors = 0;
    return lapic;
}

CPU* CreateCPUCore(int cpu_id){
    if(cpu_id < 0 || cpu_id >= MAX_CPU){
        LOG_FATAL("Invalid CPU id %d\n", cpu_id);
        return NULL;
    }
    CPU *cpu= malloc(sizeof(CPU));
    if(!cpu){
        LOG_FATAL("Memory allocation failed for CPU %d\n",
               cpu_id);
        return NULL;
    }
    cpu->id = cpu_id;
    cpu->lapic = CreateLAPIC(cpu_id, cpu);
    if(!cpu->lapic){
        free(cpu);
        LOG_FATAL("Failed to create LAPIC for CPU %d\n", cpu_id);
        return NULL;
    }
    cpu->running = 0;
    return cpu;
}

CPU_CONTEXT* CreateCpuContext(int id){
    if(id < 0 || id >= MAX_CPU){
        LOG_FATAL("Invalid CPU id %d in CreateCpuContext\n", id);
        return NULL;
    }
    CPU *cpu = CreateCPUCore(id);
    if(!cpu){
        LOG_FATAL("Failed to create CPU core %d\n", id);
        return NULL;
    }
    CPU_CONTEXT *ctx = calloc(1, sizeof(CPU_CONTEXT));
    if(!ctx){
        free(cpu->lapic);
        free(cpu);
        LOG_FATAL("CPU_CONTEXT allocation failed for CPU %d\n", id);
        return NULL;
    }
    ctx->cpu = cpu;
    ctx->irql = PASSIVE_LVL;
    ctx->dpc_head = NULL;
    ctx->dpc_tail = NULL;
    return ctx;
}

DEVICE* CreateDevice(const char *name, const MSI_CAP *msi_cap){
    if(!name || !msi_cap){
        LOG_FATAL("Invalid parameters to CreateDevice\n");
        return NULL;
    }
    DEVICE *device = calloc(1, sizeof(DEVICE));
    if(!device){
        LOG_FATAL("Failed to create DEVICE %s\n", name);
        return NULL;
    }
    strncpy(device->name, name, sizeof(device->name) - 1);
    device->name[sizeof(device->name) - 1] = '\0';
    device->msi_cap = msi_cap;
    device->msi.enabled = 0;
    device->use_msi = 0;
    device->irq.irq_line = -1;
    device->buffer.head = device->buffer.tail = 0;
    return device;
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
    if(!device || !cpu){
        LOG_FATAL("Invalid device or CPU in ConfigureDevice\n");
        return;
    }
    if(cpu->id < 0 || cpu->id >= MAX_CPU){
        LOG_FATAL("CPU id %d out of range\n", cpu->id);
        return;
    }
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
    ConfigureDevice(microphone, g_sys.cpus[3], 35);
    g_sys.devices[g_sys.d_count++] = microphone;
}

void IoApicHandleIRQ(IOAPIC *ioapic, int irq){
    if(irq < 0 || irq >= MAX_IRQ){
        LOG_KERN("[IOAPIC] Invalid IRQ %d\n", irq);
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
            LOG_KERN("[Device: %s] Vector %d too large for bitmap\n",
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

void DeviceGenerateEvent(DEVICE *device, int value){
    if(!device){
        LOG_FATAL("NULL device cannot generate event.\n");
        return;
    }
    RB_Push(&device->buffer, value);
    DeviceRaiseInterrupt(device);
}

typedef struct{
    DEVICE *device;
} ISR_CONTEXT;

typedef int (*fIsrHandlerTable)(CPU_CONTEXT *cpu_ctx, ISR_CONTEXT *isr_ctx);

typedef struct _ISR_NODE{
    fIsrHandlerTable fhandler;
    void *ctx;
    struct _ISR_NODE *next;
} ISR_NODE;

ISR_NODE *g_isr_table[MAX_VECTOR];

typedef void (*fDpcHandler)(void *ctx);

typedef struct _DPC{
    fDpcHandler func;
    void *ctx;
    struct _DPC *next;
} DPC;

void QueueDpc(CPU_CONTEXT *cpu_ctx, fDpcHandler func, void *data){
    DPC *dpc = malloc(sizeof(DPC));
    if(!dpc) return;
    dpc->func = func;
    dpc->ctx = data;
    dpc->next = NULL;

    if(cpu_ctx->dpc_tail){
        cpu_ctx->dpc_tail->next = dpc;
    }else{
        cpu_ctx->dpc_head = dpc;
    }
    cpu_ctx->dpc_tail = dpc;                  
}

void ProcessDpcQueue(CPU_CONTEXT *cpu_ctx){
    if(!cpu_ctx->dpc_head) return;
    IRQL oldIrql = RaiseIrql(cpu_ctx, DISPATCH_LVL);
    while(cpu_ctx->dpc_head){
        DPC *dpc = cpu_ctx->dpc_head;
        cpu_ctx->dpc_head = dpc->next;
        if(cpu_ctx->dpc_head == NULL)
            cpu_ctx->dpc_tail = NULL;

        dpc->func(dpc->ctx);
        free(dpc);
    }
    LowerIrql(cpu_ctx, oldIrql);
}

void RegisterIsr(int vector, fIsrHandlerTable handler, DEVICE *device){
    if(vector < 0 || vector >= MAX_VECTOR){
        LOG_FATAL("Invalid vector %d in RegisterIsr\n", vector);
        return;
    }
    if(!handler){
        LOG_FATAL("Handler is NULL for vector %d\n", vector);
        return;
    }
    ISR_NODE *node = malloc(sizeof(ISR_NODE));
    ISR_CONTEXT *isr_ctx = malloc(sizeof(ISR_CONTEXT));
    if(!node || !isr_ctx){
        LOG_FATAL("Memory allocation failed at Register ISR function");
        free(node);
        free(isr_ctx);
        return;
    }
    isr_ctx->device = device;  
    node->fhandler = handler;
    node->ctx = isr_ctx;
    node->next = g_isr_table[vector];
    g_isr_table[vector] = node;
}
    
int LapicGetNextVector(LAPIC *lapic){
    if(!lapic){
        LOG_FATAL("LapicGetNextVector called with NULL LAPIC\n");
        return -1;
    }
    if(lapic->pending_vectors == 0) return -1;
    int vector = FindLowestSetBit(lapic->pending_vectors);
    if(vector < 0 || vector >= 64){
        LOG_FATAL("Invalid vector %d in LAPIC\n", vector);
        return -1;
    }
    lapic->pending_vectors &= ~(1ULL << vector);
    lapic->in_service_vector = vector;
    return vector; 
}

void LapicSendEoi(LAPIC *lapic){
    lapic->in_service_vector = -1;
}

int CommonInterruptEntry(void *ctx){
    CPU_CONTEXT *cpu_ctx = (CPU_CONTEXT *)ctx;
    CPU *cpu = cpu_ctx->cpu;
    int vector = cpu->lapic->in_service_vector;

    ISR_NODE *node = g_isr_table[vector];
    if(!node){
        printf("[CPU %d] Unhandled interrupt vector %d\n",
               cpu->id, vector);
        return 0;
    }
    printf("[CPU %d] Dispatching vector %d\n",
               cpu->id, vector);

    while(node){
        ISR_CONTEXT *isr_ctx = (ISR_CONTEXT *)node->ctx;
        if(node->fhandler(cpu_ctx, isr_ctx) == HANDLED) break;
        node = node->next;
    }
    return 1;
}

void RegisterInterrupt(int vector, fHandler handler){
    g_idt[vector].handler = handler;
}

void DispatchInterrupt(CPU_CONTEXT *cpu_ctx, int vector){
    IRQL oldIrql = RaiseIrql(cpu_ctx, DEVICE_LVL);
    CPU *cpu = cpu_ctx->cpu;
    cpu->lapic->in_service_vector = vector;
    if(g_idt[vector].handler){
        g_idt[vector].handler(cpu_ctx);
    }else{
        printf("[CPU %d] No IDT entry for vector %d\n",
               cpu->id, vector);
    }
    LapicSendEoi(cpu->lapic);
    LowerIrql(cpu_ctx, oldIrql);
}

void CpuStep(CPU_CONTEXT *cpu_ctx){
     if(!cpu_ctx || !cpu_ctx->cpu || !cpu_ctx->cpu->lapic){
        LOG_FATAL("Invalid CPU context in CpuStep\n");
        return;
    }
    CPU *cpu = cpu_ctx->cpu;
    int vector;
    while((vector = LapicGetNextVector(cpu->lapic)) != -1){
        DispatchInterrupt(cpu_ctx, vector);
    }
    ProcessDpcQueue(cpu_ctx);
}

//Night work:
//Simple for now
int RaiseIrql(CPU_CONTEXT *cpu_ctx, IRQL newIrql){
    IRQL old = cpu_ctx->irql;
    if(newIrql > cpu_ctx->irql)
        cpu_ctx->irql = newIrql;
    
    return old;   
}

void LowerIrql(CPU_CONTEXT *cpu_ctx, IRQL oldIrql){
    cpu_ctx->irql = oldIrql;
}

//These are dirvers
void KeyboardDpc(void *data){
    DEVICE *device = (DEVICE *)data;
    int value;
    while(RB_Pop(&device->buffer, &value)){
        printf("[DPC] Keyboard processed key: %d\n", value);
    }  
}

int KeyboardIsr(CPU_CONTEXT *cpu_ctx, ISR_CONTEXT *isr_ctx){
    printf("[ISR] Keyboard interrupt received\n");
    QueueDpc(cpu_ctx, KeyboardDpc, isr_ctx->device);
    return HANDLED;
}

void MicrophoneDpc(void *data){
    DEVICE *device = (DEVICE *)data;
    int value;
    while(RB_Pop(&device->buffer, &value)){
        printf("[DPC] Microphone processed key: %d\n", value);
    } 
}

int MicrophoneIsr(CPU_CONTEXT *cpu_ctx, ISR_CONTEXT *isr_ctx){
    printf("[ISR] Microphone interrupt received\n");
    QueueDpc(cpu_ctx, MicrophoneDpc, isr_ctx->device);
    return HANDLED;
}

void MouseDpc(void *data){
    DEVICE *device = (DEVICE *)data;
    int value;
    while(RB_Pop(&device->buffer, &value)){
        printf("[DPC] Mouse processed key: %d\n", value);
    }
}

int MouseIsr(CPU_CONTEXT *cpu_ctx, ISR_CONTEXT *isr_ctx){
    printf("[ISR] mouse interrupt received\n");
    QueueDpc(cpu_ctx, MouseDpc, isr_ctx->device);
    return HANDLED;
}

//Debug functions
int IsLapic_id_n_owner(LAPIC *lapic){
    if(!lapic){
        LOG_FATAL("LAPIC is NULL\n");
        return 0;
    }
    if(!lapic->owner_cpu){
        LOG_FATAL("LAPIC has no owner CPU\n");
        return 0;
    }
    int check = lapic->owner_cpu->id;
    if(check < 0 || check > 4){
        LOG_FATAL("Invalid CPU id: %d\n", check);
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
        CPU_CONTEXT *ctx = CreateCpuContext(passVal);
        g_sys.cpus[g_sys.c_count] = ctx->cpu;
        g_sys.context[g_sys.c_count] = ctx;
        g_sys.c_count++;
    }

    g_sys.ioapic = calloc(1, sizeof(IOAPIC));
    if(!g_sys.ioapic){
        LOG_HW("[HW]Memory allocation failed for IOAPIC instance");
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
    
    RegisterInterrupt(33, CommonInterruptEntry);
    RegisterInterrupt(34, CommonInterruptEntry);
    RegisterInterrupt(35, CommonInterruptEntry);
  
    RegisterIsr(33, KeyboardIsr, g_sys.devices[0]);
    RegisterIsr(34, MouseIsr, g_sys.devices[1]);
    RegisterIsr(35, MicrophoneIsr, g_sys.devices[2]);

    DeviceGenerateEvent(g_sys.devices[0], 101);
    DeviceGenerateEvent(g_sys.devices[1], 102);
    DeviceGenerateEvent(g_sys.devices[2], 103);
    DeviceGenerateEvent(g_sys.devices[0], 106);
 
    DumpLAPIC(g_sys.cpus[0]->lapic);

    int i = 0;
    while(i < g_sys.c_count){
        CpuStep(g_sys.context[i++]);
    }

    puts("System Terminated>>>");
}

int main(){
    puts("Inpterrupt Understand Gaining Phase:");
    SystemInit();
    return 0;
}
