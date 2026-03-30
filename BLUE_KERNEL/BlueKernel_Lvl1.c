/* Rules to keep in mind:
   1. Scheduling can only occur when: IRQL < DISPATCH_LEVEL
   2. DPC runs at DISPATCH_LEVEL and blocks scheduling.
   3. Timer interrupt drives quantum expiration.
   4. ISR cannot directly switch threads.
   It must: Queue DPC and DPC performs scheduling work

   Now after thrid session we have came across Priority inversion.
   Priority inversion exists if:
   1. A high-priority thread `H` is Blocked
   2. It is blocked on a mutex owned by `L` (low-priority thread)
   3. There exists a READY/RUNNING thread `M` (Midum-priority thread)
   4. priority(M) > Priority(L)
   If all four are true -> inversion is active.
   This is exactly the scenario that caused the failure on the <Mars Pathfinder>.
   We successfully prevent Priority inversion.
*/

#include <stdio.h>
#include <stdlib.h>

#define MAX_QUEUE 16
#define MAX_NESTING 8
#define MAX_THREADS 8

typedef enum{
    PASSIVE_LEVEL = 0,
    APC_LEVEL,
    DISPATCH_LEVEL,
    DIRQL,
    KEYBOARD,
    MACHINE_CHECK
    //IRQL_COUNT
} IRQL;

IRQL irql_stack[MAX_NESTING];
int irql_top = -1;

int reschedule_requested = 0;

typedef struct{
    IRQL current_irql;
} CPU;

CPU cpu = {PASSIVE_LEVEL};

//Simulated BugCheck
void KernelBugcheck(const char *reason){
    puts("\n*** KERNEL BUGCHECK ***");
    printf("Reason: %s\n", reason);
    exit(1);
}

//Wait Simulation
void kernel_wait(){
    if(cpu.current_irql > PASSIVE_LEVEL){
        KernelBugcheck("Wait attempted above PASSIVE_LEVEL");
    }
    puts("Thread waited successfully");
}

//Pageable Memory Simulation
int isPageable(void *address){
    //For simulation: odd addresses = pageable
    return ((uintptr_t)address % 2);
}

void access_memory(void *address){
    if(isPageable(address) && cpu.current_irql > APC_LEVEL){
        KernelBugcheck("Page fault at elevated IRQL");
    }
    printf("Memory accessed safely at IRQL %d\n", cpu.current_irql);
}

//Deffered Procedure Calls at Dispatch Level 
typedef void (*DPC_ROUTINE)(void);

typedef struct{
    DPC_ROUTINE routine;
} DPC;

DPC dpc_queue[MAX_QUEUE];
int dpc_head = 0;
int dpc_tail = 0;
int dpc_count = 0;

void queue_dpc(DPC_ROUTINE routine){
    if(dpc_count >= MAX_QUEUE){
        KernelBugcheck("DPC queue overflow");
    }
    dpc_queue[dpc_tail].routine = routine;
    dpc_tail = (dpc_tail + 1) % MAX_QUEUE;
    dpc_count++;
    printf("DPC queued\n");
}

void drain_dpc_queue(){
    if(cpu.current_irql != DISPATCH_LEVEL)
        return;

    puts("Drainig DPC queue...");

    while(dpc_count > 0){
        DPC_ROUTINE routine = dpc_queue[dpc_head].routine;
        dpc_head = (dpc_head + 1) % MAX_QUEUE;
        dpc_count--;
        routine();
    }
}

//IRQL Control
/* void raise_irql(IRQL new_level){ */
/*     if(new_level < cpu.current_irql){ */
/*         kernel_bugcheck("Invalid IRQL raise"); */
/*     } */
/*     cpu.current_irql = new_level; */
/*     printf("IRQL raise to %d\n", cpu.current_irql); */
/* } */

/* void lower_irql(IRQL new_level){ */
/*     if(new_level > cpu.current_irql){ */
/*         kernel_bugcheck("Invalid IRQL lower"); */
/*     } */
/*     cpu.current_irql = new_level; */
/*     printf("IRQL lowered to %d\n", cpu.current_irql); */

/*     if(cpu.current_irql == DISPATCH_LEVEL) */
/*         drain_dpc_queue(); */
/* } */

//interrupt Simulation
void push_irql(IRQL new_level){
    if(irql_top >= MAX_NESTING - 1)
        KernelBugcheck("IRQL stack overflow");

    if(irql_top >= 0 && new_level <= irql_stack[irql_top])
        KernelBugcheck("Invalid IRQL raise (not higher)");

    irql_stack[++irql_top] = new_level;
    cpu.current_irql = new_level;

    printf("IRQL pushed -> %d\n", cpu.current_irql);
}

void pop_irql(){
    if(irql_top < 0)
        KernelBugcheck("IRQL stack underflow");

    IRQL old_level = cpu.current_irql;

    irql_top--;

    if(irql_top >= 0)
        cpu.current_irql = irql_stack[irql_top];
    else
        cpu.current_irql = PASSIVE_LEVEL;

    printf("IRQL popped -> %d\n", cpu.current_irql);

    if(old_level > DISPATCH_LEVEL && cpu.current_irql < DISPATCH_LEVEL){
        cpu.current_irql = DISPATCH_LEVEL;
        drain_dpc_queue();
        cpu.current_irql = PASSIVE_LEVEL;
    }   
}

typedef void (*ISR_ROUTINE)(void);

typedef struct{
    IRQL irql;
    ISR_ROUTINE isr;
} INTERRUPT;

//Before nesting logic
/* void trigger_interrupt(INTERRUPT *interrupt){ */
/*     if(interrupt->irql <= cpu.current_irql){ */
/*         puts("Interrupt masked"); */
/*         return; */
/*     } */
/*     raise_irql(interrupt->irql); */
/*     puts("ISR executing..."); */
/*     //kernel_wait(); //wait attempted at Dispatch level */
/*     interrupt->isr(); */
    
/*     trigger_interrupt(interrupt);//interrupt masked */
/*     lower_irql(DISPATCH_LEVEL); */
/*     lower_irql(PASSIVE_LEVEL); */
/*     //kernel_wait(); //successful because at 0 waiting is valid */
/* } */
void trigger_interrupt(INTERRUPT *interrupt){
    if(irql_top >= 0 && interrupt->irql <= cpu.current_irql){
        printf("Interrupt masked (IRQL %d)\n", interrupt->irql);
        return;
    }

    push_irql(interrupt->irql);
    printf("ISR executing at IRQL %d\n", cpu.current_irql);
    interrupt->isr();
    pop_irql();
}

void device_dpc(){
    printf("DPC running at IRQL %d \n", cpu.current_irql);
    access_memory((void *)2);
}

void device_isr(){
    printf("ISR running at IRQL %d \n", cpu.current_irql);
    //we are treating odd memory address as pageable 
    /*
      access_memory((void *)3);
      // We get Page fault at elevated IRQL 
      */
    queue_dpc(device_dpc);
}

void high_dpc(){
    printf("High DPC running at IRQL %d\n", cpu.current_irql);
}

void high_isr(){
    printf("High ISR running at IRQL %d\n", cpu.current_irql);
    queue_dpc(high_dpc);
}

void low_isr(){
    printf("Low ISR running at IRQL %d\n", cpu.current_irql);

    INTERRUPT highInterrupt ={
        .irql = 5,
        .isr = high_isr
    };

    trigger_interrupt(&highInterrupt);

    queue_dpc(device_dpc);
}

//Now introducing THREAD and SCHEDULING
typedef enum{
    THREAD_READY = 0,
    THREAD_RUNNING,
    THREAD_BLOCKED,
    THREAD_TERMINATED
} THREAD_STATE; 

typedef struct _THREAD THREAD;
typedef void (*ThreadRoutine)(THREAD *);

typedef struct _THREAD{
    int id;
    int pc; //simulated instruction pointer
    int priority;
    int base_priority;
    int quantum;
    THREAD_STATE state;
    ThreadRoutine tRoutine;
} THREAD;

THREAD thread_table[MAX_THREADS];
int thread_count = 0;

THREAD *current_thread = NULL;

void CheckPreemption(THREAD *newThread);

void CreateThread(ThreadRoutine tdR, int priority){
    THREAD *t = &thread_table[thread_count++];
    t->id = thread_count;
    t->priority = priority;
    t->base_priority = priority;
    t->quantum = 3;
    t->state = THREAD_READY;
    t->tRoutine = tdR;

    printf("Thread %d created (priority %d) \n", t->id, priority);

    CheckPreemption(t);
}

THREAD* PickNextThread(){
    THREAD *best = NULL;

    for(int i = 0; i < thread_count; i++){
        if(thread_table[i].state == THREAD_READY){
            if(!best || thread_table[i].priority > best->priority)
                best = &thread_table[i];
        }
    }
    return best;
}

void Schedule(){
    if(cpu.current_irql >= DISPATCH_LEVEL)
        return;

    THREAD *next = PickNextThread();
    if(!next)
        return;
    
    if(current_thread == next)
        return;
    
    if(current_thread && current_thread->state == THREAD_RUNNING)
        current_thread->state = THREAD_READY;

    next->state = THREAD_RUNNING;
    current_thread = next;

    printf("Switched to thread %d\n", current_thread->id);
}

void CheckPreemption(THREAD *newThread){
    if(!current_thread)
        return;

    if(newThread->priority > current_thread->priority){
        if(cpu.current_irql < DISPATCH_LEVEL){
            printf("Immediate preemption: T%d -> T%d\n",
                   current_thread->id, newThread->id);
        }else{
            printf("Preemption defferred (IRQL %d)\n",
                   cpu.current_irql);  
        }
        reschedule_requested = 1;
    }
}

void TimerDPC(){
    printf("Timer DPC running\n");

    if(current_thread){
        current_thread->quantum--;

        if(current_thread->quantum <= 0){
            printf("Thread %d quantum expired\n", current_thread->id);
            current_thread->quantum = 3;
            reschedule_requested = 1;
        }
    }    
}

//Minimal Real Wait Queue
typedef struct _WAIT_QUEUE{
    THREAD *threads[MAX_THREADS];
    int count;
} WAIT_QUEUE;

void InitWaitQueue(WAIT_QUEUE *q){
    q->count = 0;
}

void EnqueueWaiter(WAIT_QUEUE *q, THREAD *t){
    if(q->count >= MAX_THREADS)
        KernelBugcheck("Wait queue overflow");

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

//Mutex
typedef struct _MUTEX{
    int locked;
    THREAD *owner;
    WAIT_QUEUE waiters;
} MUTEX;

MUTEX test_mutex;

void BlockCurrentThread(WAIT_QUEUE *queue);

void AcquireMutex(MUTEX *m){
    if(cpu.current_irql != PASSIVE_LEVEL)
        KernelBugcheck("AcquireMutex must occur at PASSIVE_LEVEL");

    if(!m->locked){
        m->locked = 1;
        m->owner = current_thread;
        return;
    }

    printf("Thread %d blocking on mutex.\n", current_thread->id);
    
    THREAD *owner = m->owner;
    if(owner && owner->priority < current_thread->priority){
        printf("Priority inheritance: Boosting T%d from %d to %d\n",
               owner->id,
               owner->priority,
               current_thread->priority);
        
        owner->priority = current_thread->priority;
    }
    BlockCurrentThread(&m->waiters);
}

void ReleaseMutex(MUTEX *m){
    if(m->owner != current_thread)
        KernelBugcheck("Mutex released by non-owner");

    printf("Restoring T%d priority from %d to base %d\n",
           current_thread->id,
           current_thread->priority,
           current_thread->base_priority);

    current_thread->priority = current_thread->base_priority;

    if(m->waiters.count == 0){
        m->locked = 0;
        m->owner = NULL;
        return;
    }
    THREAD *next = DequeueHighestPriority(&m->waiters);
    m->owner = next;
    next->state = THREAD_READY;
    printf("Mutex ownership transferred to T%d\n", next->id);
    CheckPreemption(next);
}

//Wake One Thread (DPC Context)
//Wake must occur at DISPATCH_LEVEL
void WakeOne(WAIT_QUEUE *queue){
    if(cpu.current_irql != DISPATCH_LEVEL)
        KernelBugcheck("Wake must occur at DISPATCH_LEVEL");

    if(queue->count == 0)
        return;
    
    THREAD *t = DequeueHighestPriority(queue);

    t->state = THREAD_READY;
    printf("Thread %d awakened\n", t->id);
    CheckPreemption(t);
}

//Block Current Thread
void BlockCurrentThread(WAIT_QUEUE *queue){
    if(cpu.current_irql != PASSIVE_LEVEL)
        KernelBugcheck("Blocking above PASSIVE_LEVEL");

    if(!current_thread)
        return;

    current_thread->state = THREAD_BLOCKED;
    /* queue->threads[queue->count++] = current_thread; */
    EnqueueWaiter(queue, current_thread);

    printf("Thread %d blocked\n", current_thread->id);
    reschedule_requested = 1;
}

typedef struct{
    int signaled; //0 = nonsignaled , 1 = signaled
    WAIT_QUEUE waiters;
} EVENT;

EVENT io_event;

void InitEvent(EVENT *e){
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

void SignalEvent(EVENT *e){
    if(cpu.current_irql != DISPATCH_LEVEL)
        KernelBugcheck("Signal must occur at DISPATCH_LEVEL");
    
    e->signaled = 1;

    if(e->waiters.count > 0){
        WakeOne(&e->waiters);
        e->signaled = 0;
    }
}

//Priority Inversion Detector
void DetectPriorityInversion(){
    for(int i = 0; i < thread_count; i++){
        THREAD *blocked = &thread_table[i];
        if(blocked->state != THREAD_BLOCKED)
            continue;

        if(test_mutex.owner){
            THREAD *owner = test_mutex.owner;
            if(blocked->priority > owner->priority){
                for(int j = 0; j < thread_count; j++){
                    THREAD *other = &thread_table[j];
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
    queue_dpc(TimerDPC);
}

void StartIoOperation(){
    push_irql(DIRQL);
    queue_dpc(IoCompletionDpc);
    pop_irql();
}

//Idle Thread to keep one thread running invariant intact 
void IdleThread(THREAD *t){
    (void)t;
    puts("Idle running...");
}

//Examples threads for test:
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
            t->state = THREAD_TERMINATED;
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
            t->state = THREAD_TERMINATED;
            reschedule_requested = 1;
            return;
        }
    }
}

void Thread_L(THREAD *t){
    switch(t->pc){
        case 0:{
            puts("L : Acquiring mutex");
            AcquireMutex(&test_mutex);
            t->pc = 1;
            return;
        }
        case 1:{
            puts("L: Holding mutex (long work)");
            t->pc = 2;
            return;
        }
        case 2:{
            puts("L: Releasing mutex");
            ReleaseMutex(&test_mutex);
            t->state = THREAD_TERMINATED;
            reschedule_requested = 1;
            return;
        }
    }
}

void Thread_H(THREAD *t){
    switch(t->pc){
        case 0:{
            puts("H: Trying to acquire mutex");
            AcquireMutex(&test_mutex);
            t->pc = 1;
            return;
        }
        case 1:{
            puts("H: Acquired mutex");
            puts("H: DONE it's work");
            ReleaseMutex(&test_mutex);
            t->state = THREAD_TERMINATED;
            t->pc = 2;
            reschedule_requested = 1;
            return;
        }
    }
}

void Thread_M(THREAD *t){
    switch(t->pc){
        case 0:{
            puts("M: running independent work.");
            t->pc = 1;
            return;
        }
        case 1:{
            puts("M: running independent work.");
            t->state = THREAD_TERMINATED;
            return;
        }       
    }
}

int main(){
    puts("KERNEL SIMULATOR:");
    puts("System start at PASSIVE\n");

    /* //check IRQL value */
    /* IRQL chkIrql; */
    /* for(int i= 0; i < IRQL_COUNT; i++){ */
    /*     chkIrql = (IRQL)i; */
    /*     printf("%d\n", chkIrql); */
    /* } */

    INTERRUPT device_interrupt = {
        .irql = DIRQL,
        .isr = low_isr
    };
    kernel_wait();
    /* raise_irql(APC_LEVEL); */

    trigger_interrupt(&device_interrupt);
    
    /* CreateThread(Thread_A, 1); */
    /* CreateThread(Thread_B, 5); */

    puts("\n");
    CreateThread(Thread_L, 1);  // Low
    CreateThread(IdleThread, 0);
    Schedule();
    current_thread->tRoutine(current_thread);
    
    CreateThread(Thread_M, 3);  // Medium
    CreateThread(Thread_H, 5);  //high
    InitEvent(&io_event);
    
    INTERRUPT timer_interrupt = {
        .irql = DIRQL,
        .isr = TimerIsr
    };

    for(int i = 0; i < 10; i++) {
        printf("\n--- Timer Tick %d ---\n", i);
        trigger_interrupt(&timer_interrupt);

        if(current_thread && current_thread->state == THREAD_RUNNING){
            current_thread->tRoutine(current_thread);
        }
        
        if(reschedule_requested && cpu.current_irql < DISPATCH_LEVEL){
            Schedule();
            reschedule_requested = 0;
            DetectPriorityInversion();
        }
    }
    puts("\nBack to PASSIVE_LEVEL");
    return 0;
}
  
