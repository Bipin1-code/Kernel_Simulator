#include <stdio.h>
#include <stdlib.h>
#include <stdint.h> 

#define MAX_QUEUE 16
#define MAX_NESTING 8
#define MAX_THREADS 16
#define MAX_PROCESSES 10
#define MAX_HANDLES 32
#define MAX_PRIORITY 32

#define DEBURIJN32 0x077CB531U
static uint32_t g_priorityTable[MAX_PRIORITY];

void BuildDeBurijnTable(){
    for(int n = 0; n < 32; n++){
        uint32_t v = 1u << n;
        uint32_t idx = (v * DEBURIJN32) >> 27;
        g_priorityTable[idx] = n;
    }
}

typedef enum _IRQL{
    PASSIVE_LEVEL = 0,
    APC_LEVEL,
    DISPATCH_LEVEL,
    DIRQL,
    KEYBOARD,
    MACHINE_CHECK
} IRQL;

IRQL irql_stack[MAX_NESTING];
int irql_top = -1;

typedef struct _CPU{
    IRQL current_irql;
} CPU;

CPU cpu = {PASSIVE_LEVEL};

//Simulated BugCheck
void KernelBugcheck(const char *reason){
    printf("\n \x1b[31m[KERNEL BUGCHECK]\x1b[0m");
    printf("Reason: %s\n", reason);
    exit(1);
}

//Wait Simulation
void KernelWait(){
    if(cpu.current_irql > PASSIVE_LEVEL){
        KernelBugcheck("Wait attempted above PASSIVE_LEVEL");
    }
    puts("Thread waited successfully");
}

//Pageable Memory Simulation
int IsPageable(void *address){
    return ((uintptr_t)address % 2);
}

void AccessMemory(void *address){
    if(IsPageable(address) && cpu.current_irql > APC_LEVEL){
        KernelBugcheck("Page fault at elevated IRQL");
    }
    printf("Memory accessed safely at IRQL %d\n", cpu.current_irql);
}

//Deferred Procedure Calls at Dispatch Level 
typedef void (*DPC_ROUTINE)(void);

typedef struct _DPC{
    DPC_ROUTINE routine;
} DPC;

DPC dpc_queue[MAX_QUEUE];
int dpc_head = 0;
int dpc_tail = 0;
int dpc_count = 0;

void QueueDpc(DPC_ROUTINE routine){
    if(dpc_count >= MAX_QUEUE){
        KernelBugcheck("DPC queue overflow");
    }
    dpc_queue[dpc_tail].routine = routine;
    dpc_tail = (dpc_tail + 1) % MAX_QUEUE;
    dpc_count++;
    printf("DPC queued\n");
}

void DrainDpcQueue(){
    if(cpu.current_irql != DISPATCH_LEVEL)
        return;

    puts("Draining DPC queue...");
    while(dpc_count > 0){
        DPC_ROUTINE routine = dpc_queue[dpc_head].routine;
        dpc_head = (dpc_head + 1) % MAX_QUEUE;
        dpc_count--;
        routine();
    }
}

//Interrupt Simulation
void PushIrql(IRQL new_level){
    if(irql_top >= MAX_NESTING - 1)
        KernelBugcheck("IRQL stack overflow");

    if(irql_top >= 0 && new_level <= irql_stack[irql_top])
        KernelBugcheck("Invalid IRQL raise (not higher)");

    irql_stack[++irql_top] = new_level;
    cpu.current_irql = new_level;

    printf("IRQL pushed -> %d\n", cpu.current_irql);
}

void PopIrql(){
    if(irql_top < 0)
        KernelBugcheck("IRQL stack underflow");

    IRQL old_level = cpu.current_irql;
    IRQL new_level;

    irql_top--;

    if(irql_top >= 0)
        new_level = irql_stack[irql_top];
    else
        new_level = PASSIVE_LEVEL;

    cpu.current_irql = new_level;

    printf("IRQL popped -> %d\n", cpu.current_irql);

    if(old_level > DISPATCH_LEVEL && new_level < DISPATCH_LEVEL){
        cpu.current_irql = DISPATCH_LEVEL;
        DrainDpcQueue();
        cpu.current_irql = new_level;
    }
}

typedef void (*ISR_ROUTINE)(void);

typedef struct _INTERRUPT{
    IRQL irql;
    ISR_ROUTINE isr;
} INTERRUPT;

void TriggerInterrupt(INTERRUPT *interrupt){
    if(interrupt->irql <= cpu.current_irql){
        printf("Interrupt masked (IRQL %d)\n", interrupt->irql);
        return;
    }

    PushIrql(interrupt->irql);
    printf("ISR executing at IRQL %d\n", cpu.current_irql);
    interrupt->isr();
    PopIrql();
}

void DeviceDpc(){
    printf("DPC running at IRQL %d \n", cpu.current_irql);
    AccessMemory((void *)2);
}

void HighDpc(){
    printf("High DPC running at IRQL %d\n", cpu.current_irql);
}

void DeviceIsr(){
    printf("ISR running at IRQL %d \n", cpu.current_irql);
    QueueDpc(DeviceDpc);
}

void HighIsr(){
    printf("High ISR running at IRQL %d\n", cpu.current_irql);
    QueueDpc(HighDpc);
}

void LowIsr(){
    printf("Low ISR running at IRQL %d\n", cpu.current_irql);

    INTERRUPT highInterrupt ={
        .irql = 5,
        .isr = HighIsr
    };
    
    TriggerInterrupt(&highInterrupt);
    QueueDpc(DeviceDpc);
}

//KERNEL OBJECT
typedef enum{
    OBJECTTYPE_MUTEX = 1,
    OBJECTTYPE_EVENT
} OBJECT_TYPE;

typedef struct _KOBJECT{
    OBJECT_TYPE type;
    int ref_count;
} KOBJECT;

typedef int HANDLE;

//[NOTE: Do not touch above code they are correctly working] 

typedef struct _THREAD THREAD;
typedef struct _PROCESS PROCESS;
typedef struct _MUTEX MUTEX;

typedef enum{
    THREAD_READY = 0,
    THREAD_RUNNING,
    THREAD_BLOCKED,
    THREAD_TERMINATED
} THREAD_STATE; 

typedef void (*ThreadRoutine)(THREAD *);

typedef struct _THREAD{
    int id;
    int pc; 
    int priority;
    int base_priority;
    int quantum;
    THREAD_STATE state;
    ThreadRoutine tRoutine;
    PROCESS *owner;
    MUTEX *owned_mutex;
    MUTEX *waiting_mutex;
} THREAD;

THREAD g_thread_table[MAX_THREADS];
int g_thread_count = 0;

THREAD *current_thread = NULL;
THREAD *idle_thread;

typedef struct _PROCESS{
    int pid;
    int exist;
    THREAD *threads[MAX_THREADS];
    int thread_count;
    KOBJECT *handle_table[MAX_HANDLES];
} PROCESS;

PROCESS process_table[MAX_PROCESSES];
int process_count = 0;

PROCESS* CreateProcess(){
    if(process_count >= MAX_PROCESSES)
        KernelBugcheck("Process table is overflow");
    
    PROCESS *p = &process_table[process_count++];
    p->pid = process_count;
    p->exist = 1; 
    p->thread_count = 0;

    for(int i = 0; i < MAX_HANDLES; i++)
        p->handle_table[i] = NULL;
    
    return p;
}

int ProcessHasLiveThreads(PROCESS *p){
    for(int i = 0; i < p->thread_count; i++){
        THREAD *t = p->threads[i];
        if(t->state != THREAD_TERMINATED)
            return 1;
    }
    return 0;
}

void CheckPreemption(THREAD *newThread);
THREAD* RemoveReadyThread(int priority);
void IdleThread(THREAD *t);
void ReadyThread(THREAD *t);

void CreateThread(PROCESS *process, ThreadRoutine tdR, int priority){
    if(g_thread_count >= MAX_THREADS)
       KernelBugcheck("Thread Table overflow");
       
    THREAD *t = &g_thread_table[g_thread_count++]; //start index from 0
    t->id = g_thread_count; //but id will start from 1 (good)
    t->pc = 0;
    t->priority = priority;
    t->base_priority = priority;
    t->quantum = 3;
    t->state = THREAD_READY;
    t->tRoutine = tdR;
    t->owner = process;

    if(tdR != IdleThread)
        ReadyThread(t);
    
    process->threads[process->thread_count++] = t;
    printf("Thread %d created (priority %d) \n", t->id, priority);
    CheckPreemption(t);
}

/*
  READY_QUEUE Invariant:
  For each priority p (Thread's priority):
   (g_ready_queues[p].count > 0)  <=>  (g_ready_bitmap & (1u << p)) != 0
  Meaning:
  - If the queues at priority p is non-empty, its bit must be set.
  - If the bit is set, the queue must be non-empty
  This invariant must always hold.
 */
typedef struct _READY_QUEUE{
    THREAD *threads[MAX_THREADS];
    int head;
    int tail;
    int count;
} READY_QUEUE;

//Ready Queue contain threads of same priority
//Stores the acutal threads
READY_QUEUE g_ready_queues[MAX_PRIORITY];

/*
  This is for masking on priority base
  Tells the scheduler which priority have threads
*/
uint32_t g_ready_bitmap = 0;

//Invariant assert
void ValidateReadyQueueInvariant(int p){
    READY_QUEUE *q = &g_ready_queues[p];
    int bit_set = (g_ready_bitmap & (1u << p)) != 0;
    if((q->count > 0) != bit_set)
        KernelBugcheck("Ready queue invariant violated");
}

void InitDispatcher(){
    for(int i = 0; i < MAX_PRIORITY; i++){
        g_ready_queues[i].count = 0;
        g_ready_queues[i].head = 0;
        g_ready_queues[i].tail = 0;
    }
    g_ready_bitmap = 0;
}

/*
  This function only job is to take thread which state is THREAD_READY
  Check it's priority,
  Store that thread on ready queue according to priority as index,
  Set the bit or map it to the global ready bitmap  (g_ready_bitmap) 
 */ 
void ReadyThread(THREAD *t){
    int p = t->priority;
    READY_QUEUE *q = &g_ready_queues[p];
    if(q->count >= MAX_THREADS)
        KernelBugcheck("Ready queue overflow");

    q->threads[q->tail] = t;
    q->tail = (q->tail + 1) % MAX_THREADS;
    q->count++;
    //set bit 
    g_ready_bitmap |= (1u << p); //mask priority
    ValidateReadyQueueInvariant(p);
}

/*
  Remove a thread from g_ready_queues at the given priority.
  The provided priority is expected to be the highest set in g_ready_bitmap.
  Updates queue state accordingly and clears the corresponding bit in
  g_ready_bitmap if the queue becomes empty.
*/
THREAD* RemoveReadyThread(int priority){
    READY_QUEUE *q = &g_ready_queues[priority];
    if(q->count == 0) return NULL;
    THREAD *t = q->threads[q->head];
    q->threads[q->head] = NULL; //just to ensure safely
    q->head = (q->head + 1) % MAX_THREADS;
    q->count--;
    //If the count becomes we must clear that priority represent set bit 
    if(q->count == 0)
        g_ready_bitmap &= ~(1u << priority);

     ValidateReadyQueueInvariant(priority);

    return t;
}

/*
  Removes a specific thread from g_ready_queues at an arbitrary priority.
  Unlike RemoveReadyThread (which dequeues from the highest priority),
  this function searches for and removes a given thread regardless of
  its position in the queue.
*/
int RemoveFromReadyQueue(THREAD *t, int priority){
    READY_QUEUE *q = &g_ready_queues[priority];
    int removed = 0;
    for(int i = 0; i < q->count; i++){
        int idx = (q->head + i) % MAX_THREADS;
        if(q->threads[idx] == t){
            removed = 1;
            //shifting
            for(int j = i; j < q->count - 1; j++){
                int from = (q->head + j + 1) % MAX_THREADS;
                int to = (q->head + j) % MAX_THREADS;
                q->threads[to] = q->threads[from];
            }
            q->tail = (q->tail - 1 + MAX_THREADS) % MAX_THREADS;
            q->count--;
            break;
        }
    }
    if(q->count == 0)
        g_ready_bitmap &= ~(1u << priority);

    return removed;
}

/*
  Helper function of BoostPriorityChain
  This function just remove thread from ready_queue,
  Insert again with new priority.
 */
void ReinsertReadyThread(THREAD *t, int oldPriority){
    if(t->state != THREAD_READY)
        return;

    int found = RemoveFromReadyQueue(t, oldPriority);
    if(!found)
       KernelBugcheck("Thread not found during reinsert");
   
    ReadyThread(t);
}

//This function is checked (working perfectly)
int FindHighestSetBit(uint32_t x){
    if(x == 0)
        return -1;

    x |= (x >> 1);
    x |= (x >> 2);
    x |= (x >> 4);
    x |= (x >> 8);
    x |= (x >> 16);

    x = (x + 1) >> 1;
    int idx = (x * DEBURIJN32) >> 27;
    return g_priorityTable[idx];
}

int FindHighestPriority(){
    if(g_ready_bitmap == 0)
        return -1;
    return FindHighestSetBit(g_ready_bitmap);
}

/*
 PickNextThread selects the next runnable thread based on priority.
 Policy:
  - Highest-priority READY thread is selected using g_ready_bitmap.
  - TERMINATED threads are skipped.
  - If no READY threads exist, the idle thread is returned.
 */
THREAD* PickNextThread(){
    while(g_ready_bitmap != 0){
        int p = FindHighestPriority(); 
        THREAD *t = RemoveReadyThread(p);
        if(t && t->state != THREAD_TERMINATED)
            return t;
    }
    return idle_thread;
}

int reschedule_requested = 0;

//Checked
void Schedule(){
    if(cpu.current_irql >= DISPATCH_LEVEL)
        return;

    printf("g_ready_bitmap = %x\n", g_ready_bitmap);
    
    THREAD *next = PickNextThread();
    
    if(!next)
        next = idle_thread;
    
    if(current_thread == next)
        return;
    
    if(current_thread && current_thread->state == THREAD_RUNNING){
        current_thread->state = THREAD_READY;
        ReadyThread(current_thread);
    }

    next->state = THREAD_RUNNING;
    current_thread = next;

    printf("Switched to thread %d\n", current_thread->id);
}

/*
  CheckPreemption is a scheduler helper invoked when a thread becomes READY.
  It determines whether the current thread should be preempted by a higher-priority thread.
  Rules:
  - Preemption is required if newThread has higher priority than current_thread.
  - However, preemption decision is only actionable when IRQL < DISPATCH_LEVEL.
  - If IRQL >= DISPATCH_LEVEL, preemption is deferred.
  Behavior:
  - Sets reschedule_requested to indicate a pending context switch.
  - Actual context switch is performed later by the scheduler at DISPATCH_LEVEL.
*/
void CheckPreemption(THREAD *newThread){
    if(!current_thread)
        return;

    if(newThread->priority > current_thread->priority){
        if(cpu.current_irql < DISPATCH_LEVEL){
            printf("Immediate preemption: T%d -> T%d\n",
                   current_thread->id, newThread->id);
        }else{
            printf("Preemption deferred (IRQL %d)\n",
                   cpu.current_irql);  
        }
        reschedule_requested = 1;
    }
}

void TimerDPC(){
    printf("Timer DPC running\n");

    if(!current_thread || current_thread == idle_thread)
        return;

    if(current_thread->state != THREAD_RUNNING)
        return;

    current_thread->quantum--;

    if(current_thread->quantum <= 0){
        printf("Thread %d quantum expired\n", current_thread->id);
        current_thread->quantum = 3;
        THREAD *old = current_thread;
        old->state = THREAD_READY;
        ReadyThread(old);
        current_thread = NULL;
        reschedule_requested = 1;
    }
}

//REFERENCE COUNTING
void ObReferenceObject(KOBJECT *obj){
    obj->ref_count++;
}

void ObDereferenceObject(KOBJECT *obj){
    obj->ref_count--;
    if(obj->ref_count == 0){
        printf("OBJECT DESTROYED (TYPE %d)\n", obj->type);
        free(obj);
    }
}

HANDLE ObInsertHandle(PROCESS *process, KOBJECT *obj){
    for(int i = 0; i < MAX_HANDLES; i++){
        if(process->handle_table[i] == NULL){
            process->handle_table[i] = obj;
            ObReferenceObject(obj);
            return i;
        }
    }
    KernelBugcheck("Handle table full");
    return -1;
}

KOBJECT* ObReferenceObjectByHandle(PROCESS *process, HANDLE h){
    if(h < 0 || h >= MAX_HANDLES)
        return NULL;

    KOBJECT *obj = process->handle_table[h];
    if(!obj)
        return NULL;

    ObReferenceObject(obj);
    return obj;
}

void ObCloseHandle(PROCESS *process, HANDLE h){
    if(h < 0 || h >= MAX_HANDLES)
        return;

    KOBJECT *obj = process->handle_table[h];
    if(!obj)
        return;

    process->handle_table[h] = NULL;
    ObDereferenceObject(obj);
}

typedef struct _WAIT_QUEUE{
    THREAD *threads[MAX_THREADS];
    int count;
} WAIT_QUEUE;

void InitWaitQueue(WAIT_QUEUE *q){
    q->count = 0;
}

void EnqueueWaiter(WAIT_QUEUE *q, THREAD *t){
    if(q->count >= MAX_THREADS)
        KernelBugcheck("WAIT QUEUE OVERFLOW");

    q->threads[q->count++] = t;
}

THREAD* DequeueHighestPriority(WAIT_QUEUE *q){
    if(q->count == 0)
        return NULL;

    int best_index = 0;
    for(int i = 1; i < q->count; i++){
        if(q->threads[i]->priority > q->threads[best_index]->priority){
            best_index = i;
        }
    }

    THREAD *best = q->threads[best_index];
    for(int i = best_index; i < q->count-1; i++)
        q->threads[i] = q->threads[i+1];

    q->count--;
    
    return best;
}

typedef struct _MUTEX{
    KOBJECT header;
    int locked;
    THREAD *owner;
    WAIT_QUEUE waiters;
} MUTEX;

HANDLE g_hMutex1;
HANDLE g_hMutex2;

MUTEX* CreateMutexObject(){
    MUTEX *m = malloc(sizeof(MUTEX));
    if(!m)
        KernelBugcheck("Out of memory");

    m->header.type = OBJECTTYPE_MUTEX;
    m->header.ref_count = 1;
  
    m->locked = 0;
    m->owner = NULL;
    InitWaitQueue(&m->waiters);
    static int countMutex = 0;
    printf("Mutex object created %d\n", ++countMutex);
    return m;
}

/*
  Blocks the currently running thread and enqueues it on the given wait queue.
  Expected to be called from synchronization primitives (e.g., AcquireMutex,
  WaitEvent) when the thread cannot proceed.
  Responsibilities:
  - Enforce invariant: blocking is only allowed at PASSIVE_LEVEL.
  - Transition current_thread state to THREAD_BLOCKED.
  - Insert the thread into the specified WAIT_QUEUE.
  - Request rescheduling so another runnable thread can be dispatched.
*/
void BlockCurrentThread(WAIT_QUEUE *queue){
    if(cpu.current_irql != PASSIVE_LEVEL)
        KernelBugcheck("Blocking above PASSIVE_LEVEL");

    if(!current_thread)
        return;

    current_thread->state = THREAD_BLOCKED;
    EnqueueWaiter(queue, current_thread);

    printf("Thread %d blocked\n", current_thread->id);
    reschedule_requested = 1;
}

/*
  Applies priority inheritance with transitive (chain) propagation.
  - Elevates thread priority to at least newPriority.
  - Maintains ready queue ordering if the thread is schedulable.
  - Recursively propagates the boost through mutex ownership
    to prevent priority inversion across dependency chains.
*/
void BoostPriorityChain(THREAD *t, int newPriority){
    if(!t) return;
    if(t->priority >= newPriority) return;

    printf("Boosting T%d priority %d -> %d\n", t->id, t->priority, newPriority);

    int oldPriority = t->priority;
    t->priority = newPriority;

    if(t->state == THREAD_READY)
        ReinsertReadyThread(t, oldPriority);

    /*Propagate boost if this thread is blocked */
    if(t->waiting_mutex && t->waiting_mutex->owner){
        BoostPriorityChain(t->waiting_mutex->owner, newPriority);
    }
}

/*
  AcquireMutex:
  A mutex provides exclusive ownership of a resource, ensuring that only
  one thread executes a critical section at a time.
  Precondition:
  - Must be called at PASSIVE_LEVEL (may block)
  Behavior:
  - If mutex is free:
      - Acquire ownership immediately
  - If mutex is owned:
      - Current thread is blocked and added to waiters list
      - Owner may receive priority boost (priority inheritance)
  Notes:
  - Blocking is performed via BlockCurrentThread()
  - Priority inheritance is handled via BoostPriorityChain()
*/
void AcquireMutex(MUTEX *m){
    if(cpu.current_irql != PASSIVE_LEVEL)
        KernelBugcheck("AcquireMutex must occur at PASSIVE_LEVEL");
    
    if(!m->locked){
        m->locked = 1;
        m->owner = current_thread;
        current_thread->owned_mutex = m;
        return;
    }
    printf("Thread %d blocking on mutex.\n", current_thread->id);
    current_thread->waiting_mutex = m;

    THREAD *owner = m->owner;
    if(owner)
        BoostPriorityChain(owner, current_thread->priority);
    
    ObReferenceObject((KOBJECT*)m);
    BlockCurrentThread(&m->waiters);
}

/*
  ReleaseMutex:
  Releases ownership of a mutex held by the current thread.
  Precondition:
  - Must be called by the owning thread.
  Behavior:
  - Restores the current thread's priority to its base priority
    (current implementation assumes a single owned mutex).
  - If no threads are waiting:
      - Mutex is marked free.
  - If waiters exist:
      - Highest-priority waiting thread is selected.
      - Ownership is transferred directly to that thread.
      - The selected thread is moved to READY state.
  Scheduling:
  - A reschedule may be triggered if the awakened thread has
    higher priority (via CheckPreemption).
  Notes:
  - This implementation does not yet handle multiple owned mutexes.
  - Priority restoration is simplified and will be updated when
    full priority inheritance tracking is implemented.
*/
// TODO: Replace simple priority restore with RecalculatePriority()
void ReleaseMutex(MUTEX *m){
    if(!current_thread)
        KernelBugcheck("No current thread");
    
    if(m->owner != current_thread)
        KernelBugcheck("Mutex released by non-owner");

    printf("Restoring T%d priority from %d to base %d\n",
           current_thread->id,
           current_thread->priority,
           current_thread->base_priority);

    current_thread->priority = current_thread->base_priority;
    current_thread->owned_mutex = NULL;

    if(m->waiters.count == 0){
        m->locked = 0;
        m->owner = NULL;
        return;
    }
    THREAD *next = DequeueHighestPriority(&m->waiters);
    next->waiting_mutex = NULL; 
    ObDereferenceObject((KOBJECT*)m);
    m->owner = next;
    next->owned_mutex = m;
    next->state = THREAD_READY;
    ReadyThread(next);
    printf("Mutex ownership transferred to T%d\n", next->id);
    CheckPreemption(next);
}

void DestroyProcess(PROCESS *p){
    for(int i = 0; i < MAX_HANDLES; i++){
        if(p->handle_table[i]){
            ObDereferenceObject(p->handle_table[i]);
            p->handle_table[i] = NULL;
        }
    }
    p->exist = 0;
}

void TerminateThread(THREAD *t){
    if(t->owned_mutex){
        printf("Thread %d died while owning mutex\n", t->id);
        ReleaseMutex(t->owned_mutex);
        t->owned_mutex = NULL;
    }
    printf("Thread %d terminated\n", t->id);
    t->state = THREAD_TERMINATED;
    if(t == current_thread)
        reschedule_requested = 1;
}

typedef struct{
    KOBJECT header;
    int signaled;
    WAIT_QUEUE waiters;
} EVENT;

EVENT io_event;

void InitEvent(EVENT *e){
    e->header.type = OBJECTTYPE_EVENT;
    e->header.ref_count = 1;
    e->signaled = 0;
    InitWaitQueue(&e->waiters);
}

void WaitEvent(EVENT *e){
    if(cpu.current_irql != PASSIVE_LEVEL)
        KernelBugcheck("Wait above PASSIVE");
    
    if(e->signaled){
        e->signaled = 0;
        printf("Event already signaled, no block\n");        
        return;
    }
    BlockCurrentThread(&e->waiters);
}

void WakeOne(WAIT_QUEUE *queue){
    if(cpu.current_irql != DISPATCH_LEVEL)
        KernelBugcheck("Wake must occur at DISPATCH_LEVEL");

    if(queue->count == 0)
        return;
    
    THREAD *t = DequeueHighestPriority(queue);
    t->state = THREAD_READY;
    ReadyThread(t);
    printf("Thread %d awakened\n", t->id);
    CheckPreemption(t);
}

void SignalEvent(EVENT *e){
    if(cpu.current_irql > DISPATCH_LEVEL)
        KernelBugcheck("Signal must occur at DISPATCH_LEVEL");
    
    e->signaled = 1;

    if(e->waiters.count > 0){
        WakeOne(&e->waiters);
        e->signaled = 0;
    }
}

void DetectPriorityInversion(MUTEX *m){
    for(int i = 0; i < g_thread_count; i++){
        THREAD *blocked = &g_thread_table[i];
        if(blocked->state != THREAD_BLOCKED)
            continue;
        
        if(m->owner){
            THREAD *owner = m->owner;
            if(blocked->priority > owner->priority){
                for(int j = 0; j < g_thread_count; j++){
                    THREAD *other = &g_thread_table[j];
                    if((other->state == THREAD_READY ||
                        other->state == THREAD_RUNNING) &&
                       other->priority > owner->priority &&
                       other != blocked){
                        puts("!!! PRIORITY INVERSION DETECTED !!!");
                        printf("High T%d waiting on Low T%d\n",
                               blocked->id, owner->id);
                        printf("Medium T%d preventing progress\n",
                               other->id);
                        return;
                    }
                }
            }
        }
    }
}

void IoCompletionDpc(){
    printf("IO completion DPC\n");
    SignalEvent(&io_event);
}

void TimerIsr(){
    QueueDpc(TimerDPC);
}

void StartIoOperation(){
    PushIrql(DIRQL);
    QueueDpc(IoCompletionDpc);
    PopIrql();
}

void IdleThread(THREAD *t){
    (void)t;
    puts("Idle running...");
}

//These are test threads
void Thread_A(THREAD *t){
    puts("Thread A executing...");
    switch(t->pc){
        case 0:{
            puts("Step 1");
            t->pc = 1;
            return;
        }
        case 1:{
            puts("Step 2");
            t->pc = 2;
            return;
        }
        case 2:{
            puts("Done");
            TerminateThread(t);
            reschedule_requested = 1;
            return;
        }
    }
}

void Thread_B(THREAD *t){
    puts("Thread B executing...");
    switch(t->pc){
        case 0:{
            puts("Step 1");
            t->pc = 1;
            return;
        }
        case 1:{
            puts("Step 2");
            t->pc = 2;
            StartIoOperation();
            WaitEvent(&io_event);
            return;
        }
        case 2:{
            puts("Done");
            TerminateThread(t);
            reschedule_requested = 1;
            return;
        }
    }
}

void Thread_L(THREAD *t){
    KOBJECT *obj = ObReferenceObjectByHandle(t->owner, g_hMutex1);
    if(!obj) KernelBugcheck("Invalid handle");

    MUTEX *m1 = (MUTEX*)obj;
    
    switch(t->pc){
        case 0:{
            puts("L: acquiring M1");
            AcquireMutex(m1);
            t->pc = 1;
            ObDereferenceObject(obj);
            return;
        }
        case 1:{
            puts("L: holding M1 (long work)");
            t->pc = 2;
            return;
        }
        case 2:{
            puts("L: releasing M1");
            ReleaseMutex(m1);
            TerminateThread(t);
            reschedule_requested = 1;
            ObDereferenceObject(obj);
            return;
        }
    }
}

void Thread_M(THREAD *t){
    KOBJECT *obj1 = ObReferenceObjectByHandle(t->owner, g_hMutex1);
    KOBJECT *obj2 = ObReferenceObjectByHandle(t->owner, g_hMutex2);

    if(!obj1 || !obj2)
        KernelBugcheck("Invalid handle");

    MUTEX *m1 = (MUTEX*)obj1;
    MUTEX *m2 = (MUTEX*)obj2;

    switch(t->pc){
        case 0:{
            puts("M: acquiring M2");
            AcquireMutex(m2);
            t->pc = 1;
            break;
        }
        case 1:{
            puts("M: trying to acquire M1");
            AcquireMutex(m1);   // blocks here
            t->pc = 2;
            break;
        }
        case 2:{
            puts("M: releasing M2");
            ReleaseMutex(m2);
            TerminateThread(t);
            reschedule_requested = 1;
            break;
        }
    }
    ObDereferenceObject(obj1);
    ObDereferenceObject(obj2);
}

void Thread_H(THREAD *t){
    KOBJECT *obj = ObReferenceObjectByHandle(t->owner, g_hMutex2);
    if(!obj) KernelBugcheck("Invalid handle");

    MUTEX *m2 = (MUTEX*)obj;
    switch(t->pc){
        case 0:{
            puts("H: trying to acquire M2");
            AcquireMutex(m2);
            t->pc = 1;
            ObDereferenceObject(obj);
            return;
        }
        case 1:{
            puts("H: acquired M2");
            ReleaseMutex(m2);
            TerminateThread(t);
            reschedule_requested = 1;
            ObDereferenceObject(obj);
            return;
        }
    }
}

int main(){
    puts("KERNEL SIMULATOR:");
    BuildDeBurijnTable();
    InitDispatcher();
    puts("System start at PASSIVE\n");

    INTERRUPT device_interrupt = {
        .irql = DIRQL,
        .isr = LowIsr
    };

    KernelWait();
    TriggerInterrupt(&device_interrupt);
    
    puts("\n");
    PROCESS *user = CreateProcess();
    PROCESS *sys = CreateProcess();

    MUTEX *m1 = CreateMutexObject();
    MUTEX *m2 = CreateMutexObject();
    g_hMutex1 = ObInsertHandle(user, (KOBJECT*)m1);
    g_hMutex2 = ObInsertHandle(user, (KOBJECT*)m2);
    
    puts("\n");
    CreateThread(sys, IdleThread, 1);
    idle_thread = &g_thread_table[g_thread_count - 1];
    CreateThread(user, Thread_L, 1);
    Schedule();
    current_thread->tRoutine(current_thread);
    
    CreateThread(user, Thread_M, 3);
    Schedule();
    current_thread->tRoutine(current_thread);
    CreateThread(user, Thread_H, 5);
    InitEvent(&io_event);
    
    INTERRUPT timer_interrupt = {
        .irql = DIRQL,
        .isr = TimerIsr
    };

    for(int i = 0; i < 11; i++){
        printf("\n--- Timer Tick %d ---\n", i);
        TriggerInterrupt(&timer_interrupt);
        
        if(cpu.current_irql < DISPATCH_LEVEL){
            if(reschedule_requested){
                Schedule();
                reschedule_requested = 0;              
            }
            DetectPriorityInversion(m1);
            DetectPriorityInversion(m2);
        }
                   
        if(current_thread && current_thread->state == THREAD_RUNNING){
            current_thread->tRoutine(current_thread);
        } 
    
        if(user->exist && !ProcessHasLiveThreads(user)){
            printf("Process %d exiting( All Threads terminated).\n", user->pid);
            DestroyProcess(user);
        }
    }

    puts("\nBack to PASSIVE_LEVEL");
    return 0;
}
