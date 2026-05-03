 
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#define EXP R15 //Dedicated Expected value register
#define MAIN_MEMORY_SIZE 1024
#define CACHE_LINES 8 //lines per core
#define NUM_CORES 2
#define CACHE_LINE_DATA_WORDS 2
#define STORE_BUFFER_SIZE 4

typedef uint32_t INS; 

typedef enum _REGISTER{
    R0 = 0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10,R11,
    R12, R13, R14, R15
} REGISTER;

typedef enum _MICRO_OP{
    FETCH, DECODE,
    LOAD_ADDR, LOAD_MEM, LOAD_WB,
    STORE_ADDR, STORE_MEM,
    LIC_WB,
    ADD_EXEC, ADD_WB,
    SUB_EXEC, SUB_WB,
    CMP_EXEC,
    JMP_EXEC, JEQ_EXEC, JNE_EXEC, JLT_EXEC, JGT_EXEC,
    LOCK_INC_ADDR, LOCK_INC_LOAD, LOCK_INC_STORE,
    CMPXCHG_ADDR, CMPXCHG_LOAD, CMPXCHG_COMPARE, CMPXCHG_STORE, CMPXCHG_FAIL,
    FENCE_EXEC,
    HALT_EXEC
} MICRO_OP; 

typedef enum _OPCODE{
    LOAD = 0, STORE, LIC, ADD, SUB,
    CMP, JMP, JEQ, JNE, JLT, JGT,
    LOCK_INC, CMPXCHG,
    FENCE,
    HALT
} OPCODE;

typedef enum _MESI_STATE{
    MESI_INVALID,
    MESI_EXCLUSIVE,
    MESI_SHARED,
    MESI_MODIFIED
} MESI_STATE;

typedef struct _CACHE_LINE{
    uint32_t address;
    uint32_t data[CACHE_LINE_DATA_WORDS];
    MESI_STATE state;
    bool dirty;
} CACHE_LINE;

typedef struct _CACHE{
    CACHE_LINE lines[CACHE_LINES];
} CACHE;

typedef struct _STORE_BUFFER_ENTRY{
    uint32_t addr;
    uint32_t data;
    bool valid;
} STORE_BUFFER_ENTRY;

typedef struct _STORE_BUFFER{
    STORE_BUFFER_ENTRY entries[STORE_BUFFER_SIZE];
    int head;
    int tail;
    int count;
} STORE_BUFFER;

typedef struct _CORE{
    uint32_t regs[16];
    uint32_t pc;
    bool halted;
    bool zero_flag;
    bool sign_flag;
    uint32_t cmp_result;
    uint32_t core_id;

    MICRO_OP micro_op;
    INS current_instr;
    uint8_t opc;
    uint8_t dest;
    uint8_t src;
    uint8_t mode;
    uint16_t imm;
    uint32_t addr;
    uint32_t temp_val;

    CACHE cache;
    STORE_BUFFER store_buffer;
} CORE;

typedef struct _BUS{
    uint8_t mem[MAIN_MEMORY_SIZE];
    bool locked;
    uint32_t locked_owner;

    bool bus_rd;
    bool bus_rdx;
    uint32_t snoop_addr; 
    uint32_t snoop_core_id; 
    uint32_t snoop_data[CACHE_LINE_DATA_WORDS]; //data from responding cache
    bool snoop_hit; //Did any cache respond?
} BUS;

typedef void (*fHandler)(CORE *c, BUS *bus, INS unstr);

// INS = [ opcode(8) | dst(4) | src(4) | mode(2) | immediate/addr(14) ]
INS Encode(OPCODE opc, uint8_t dest, uint8_t src,
           uint8_t mode, uint16_t imm){
    INS instr = 0x00000000;
    instr |= ((opc & 0xFF) << 24)
        | ((dest & 0x0F) << 20)
        | ((src & 0x0F) << 16)
        | ((mode & 0x03) << 14)
        | (imm & 0x3FFF);

    return instr;
}

void Decode(INS instr, uint8_t *opc, uint8_t *dest, uint8_t *src,
            uint8_t *mode, uint16_t *imm){
    *opc  = (instr >> 24) & 0xFF;
    *dest = (instr >> 20) & 0x0F;
    *src  = (instr >> 16) & 0x0F;
    *mode = (instr >> 14) & 0x03;
    *imm  =  instr & 0x3FFF;
}

//Here we define all OPCODEs
void LoadFunction(CORE *c, BUS *bus, INS instr){
    uint8_t opc, dest, src, mode;
    uint16_t imm;
    Decode(instr, &opc, &dest, &src, &mode, &imm);
    if(mode == 0){
        //Direct: address is in imm field
        c->regs[dest] = *(uint32_t *)(&bus->mem[imm]);
    }else{
        //Indirect: address is in register src
        uint32_t addr = c->regs[src];
        c->regs[dest] = *(uint32_t *)(&bus->mem[addr]);
    }
}

void StoreFunction(CORE *c, BUS *bus, INS instr){
    uint8_t opc, val_reg, addr_reg, mode;
    uint16_t imm;
    Decode(instr, &opc, &val_reg, &addr_reg, &mode, &imm);
    if(mode == 0){
        *(uint32_t *)(&bus->mem[imm]) = c->regs[val_reg];   
    }else{
        uint32_t addr = c->regs[addr_reg];
        *(uint32_t *)(&bus->mem[addr]) = c->regs[val_reg];
    }
}

void LicFunction(CORE *c, BUS *bus, INS instr){
    (void)bus;
    uint8_t opc, dest, src, mode;
    uint16_t imm;
    Decode(instr, &opc, &dest, &src, &mode, &imm);
    c->regs[dest] = imm;
}

void AddFunction(CORE *c, BUS *bus, INS instr){
    (void)bus;
    uint8_t opc, dest, src, mode;
    uint16_t imm;
    Decode(instr, &opc, &dest, &src, &mode, &imm);
    c->regs[dest] = c->regs[dest] + c->regs[src];
}

void SubFunction(CORE *c, BUS *bus, INS instr){
    (void)bus;
    uint8_t opc, dest, src, mode;
    uint16_t imm;
    Decode(instr, &opc, &dest, &src, &mode, &imm);
    c->regs[dest] = c->regs[dest] - c->regs[src];
}

void HaltFunction(CORE *c, BUS *bus, INS instr){
    (void)bus;
    (void)instr;
    c->halted = true;
}

void CompareFunction(CORE *c, BUS *bus, INS instr){
    (void)bus;
    uint8_t opc, dest, src, mode;
    uint16_t imm;
    Decode(instr, &opc, &dest, &src, &mode, &imm);

    uint32_t a = c->regs[dest];
    uint32_t b = c->regs[src];
    c->cmp_result = a - b;
    c->zero_flag = (a == b);
    c->sign_flag = (a < b);
}

void JmpFunction(CORE *c, BUS *bus, INS instr){
    (void)bus;
    uint8_t opc, dest, src, mode;
    uint16_t imm;
    Decode(instr, &opc, &dest, &src, &mode, &imm);
    if(mode == 0){
        c->pc = imm;
    }else{
        c->pc = c->regs[src];
    }
}

void JeqFunction(CORE *c, BUS *bus, INS instr){
    (void)bus;
    if(c->zero_flag){
        JmpFunction(c, bus, instr);
    }
}

void JneFunction(CORE *c, BUS *bus, INS instr){
    (void)bus;
    if(!c->zero_flag){
        JmpFunction(c, bus, instr);
    }
}

void JltFunction(CORE *c, BUS *bus, INS instr){
    (void)bus;
    if(c->sign_flag){
        JmpFunction(c, bus, instr);
    }
}

void JgtFunction(CORE *c, BUS *bus, INS instr){
    (void)bus;
    if(!c->zero_flag){
        JmpFunction(c, bus, instr);
    }
}

void LockIncFunction(CORE *c, BUS *bus, INS instr){
    uint8_t opc, dest, src, mode;
    uint16_t imm;
    Decode(instr, &opc, &dest, &src, &mode, &imm);    
    uint32_t addr;
    if(mode == 0){
        addr = imm;
    }else{
        addr = c->regs[src];
    }
    bus->locked = true;
    uint32_t val = *(uint32_t *)(&bus->mem[addr]);
    val++; //4 byte increment
    *(uint32_t *)(&bus->mem[addr]) = val;
    bus->locked = false;
}

void CmpxChgFunction(CORE *c, BUS *bus, INS instr){
    uint8_t opc, dest, src, mode;
    uint16_t imm;
    Decode(instr, &opc, &dest, &src, &mode, &imm);
    uint32_t addr = c->regs[src];
    uint32_t expected = c->regs[EXP]; //EXP = expected value register
    uint32_t new_val = c->regs[dest];
    /* bus->locked = true; */
    uint32_t current = *(uint32_t *)(&bus->mem[addr]);
    if(current == expected){
        *(uint32_t *)(&bus->mem[addr]) = new_val;
        c->zero_flag = 1; 
    }else{
        c->regs[EXP] = current;
        c->zero_flag = 0;
    }
    /* bus->locked = false; */
    printf("Core %d: EXP=%d, new=%d, mem=%d -> %s\n",
           c->core_id,
           c->regs[EXP],
           c->regs[dest],
           current,
           c->zero_flag ? "SUCCESS" : "RETRY");
}

//OPCODE Function registeration 
fHandler handlers[] = {
    [LOAD]     = LoadFunction,
    [STORE]    = StoreFunction,
    [LIC]      = LicFunction,
    [ADD]      = AddFunction,
    [SUB]      = SubFunction,
    [CMP]      = CompareFunction,
    [JMP]      = JmpFunction,
    [JEQ]      = JeqFunction,
    [JNE]      = JneFunction,
    [JLT]      = JltFunction,
    [JGT]      = JgtFunction,
    [LOCK_INC] = LockIncFunction,
    [CMPXCHG]  = CmpxChgFunction,
    /* [FENCE] */
    [HALT]     = HaltFunction
};

void CacheWriteDirect(CORE *c, uint32_t addr, uint32_t value,
                      BUS *bus, CORE *all_c, int num_c);

void CacheSnoopOtherCores(CORE *all_c, int num_cores, uint32_t core_id,
                          uint32_t addr, bool is_write, BUS *bus);

//Store Buffer functions
void StoreBufferPush(CORE *c, uint32_t addr, uint32_t data){

    //Debug if Block
    if (c->core_id == 321 && addr == 0x300) {
        printf("!!! PUSH on Core 321: addr=0x300 value=%d\n", data);
    }
    
    if(c->store_buffer.count >= STORE_BUFFER_SIZE){
        printf("Core %d: Store buffer full, forced drain\n",
               c->core_id);
        return;
    }
    int tail = c->store_buffer.tail;
    STORE_BUFFER_ENTRY *entry = &c->store_buffer.entries[tail];
    entry->addr = addr;
    entry->data = data;
    entry->valid = true;
    c->store_buffer.tail = (tail + 1) % STORE_BUFFER_SIZE;

    c->store_buffer.count++;
}

void DrainStoreBuffer(CORE *c, BUS *bus, CORE *all_c, int num_c){
    if(c->store_buffer.count == 0) return;

    printf("[Debug:]!!! CORE %d: Draining store buffer entry: addr=0x%X value=%d\n",
           c->core_id, 
           c->store_buffer.entries[c->store_buffer.head].addr,
           c->store_buffer.entries[c->store_buffer.head].data);
    
    
    int head = c->store_buffer.head;    
    STORE_BUFFER_ENTRY *entry = &c->store_buffer.entries[head];
    CacheWriteDirect(c, entry->addr, entry->data, bus, all_c, num_c);
    entry->valid = false;
    c->store_buffer.head = (head + 1) % STORE_BUFFER_SIZE;
    c->store_buffer.count--;
}

void FlushStoreBuffer(CORE *c, BUS *bus, CORE *all_c, int num_c){
    while(c->store_buffer.count > 0){
        DrainStoreBuffer(c, bus, all_c, num_c);
    }
}

//Cache associate functions
//search cache for an address. Return index (0-7) or -1 if not found.
int CacheFind(CACHE *cache, uint32_t addr){
    uint32_t line_base = addr & ~(CACHE_LINES - 1);
    for(int i = 0; i < CACHE_LINES; i++){
        if((cache->lines[i].state != MESI_INVALID) &&
           (cache->lines[i].address == line_base)){
            return i;
        }
    }
    return -1;
}

/* //old one but I will use it later for other test */
/* //Pick a victim line. Write-back if dirty. Return the freed index. */
/* int CacheEvict(CORE *c, BUS *bus){ */
/*     static int victim = 0; */
/*     int idx = victim; */
/*     victim = (victim + 1) % CACHE_LINES; //UPDATE THE VICTIM (DSA: RING array) */
/*     CACHE_LINE *line = &c->cache.lines[idx]; */

/*     if(line->state == MESI_MODIFIED){ */
/*         *(uint32_t *)(&bus->mem[line->address]) = line->data; */
/*     } */
/*     line->state = MESI_INVALID; */
/*     line->address = 0; */
/*     return idx; */
/* } */

//new one 
static void CacheEvictLine(CACHE *cache, BUS *bus, int idx){
    if(cache->lines[idx].state == MESI_MODIFIED){
        uint32_t base = cache->lines[idx].address;
        for(int w = 0; w < CACHE_LINE_DATA_WORDS; w++){
            *(uint32_t *)(&bus->mem[base + (w * 4)]) =
                cache->lines[idx].data[w];
        }
    }
    cache->lines[idx].state = MESI_INVALID;
    cache->lines[idx].address = 0;
}

static int CacheGetEmpty(CACHE *cache, BUS *bus){
    for(int i = 0; i < CACHE_LINES; i++){
        if(cache->lines[i].state == MESI_INVALID) return i;
    }
    CacheEvictLine(cache, bus, 0);
    return 0;
}

void CacheSnoop(CACHE *cache, uint32_t addr, bool is_write, BUS *bus){
    int idx = CacheFind(cache, addr);
    printf("  SNOOP core? addr=0x%X -> idx=%d\n", addr, idx);
    if(idx == -1) return;
    CACHE_LINE *line = &cache->lines[idx];
    if(is_write){
        //BusRdx: other cores wants exclusive write access
        if(line->state == MESI_MODIFIED){
            //must show the dirty data
            for(int w = 0; w < CACHE_LINE_DATA_WORDS; w++){
                bus->snoop_data[w] = line->data[w];
            }
            bus->snoop_hit = true;
        }
        line->state = MESI_INVALID;
    }else{
        //BusRd: other core wants to read
        if(line->state != MESI_INVALID){
            for(int w = 0; w < CACHE_LINE_DATA_WORDS; w++){
                bus->snoop_data[w] = line->data[w];
            }
            bus->snoop_hit = true;
        
            if(line->state == MESI_MODIFIED){
                line->state = MESI_SHARED;
            }else if(line->state == MESI_EXCLUSIVE){
                line->state = MESI_SHARED;
            }
        }
    }
}

uint32_t CacheLoad(CORE *c, uint32_t addr, BUS *bus, CORE *all_c, int num_c){
    int idx = CacheFind(&c->cache, addr);
    printf("CORE %d: LOAD 0x%X -> idx=%d\n", c->core_id, addr, idx);
    if(idx != -1){
        /* return; */
        //`chge
        int word_offset = (addr & (CACHE_LINES - 1)) / 4;
        return c->cache.lines[idx].data[word_offset];
    }

    //Miss - get a line
    uint32_t line_base = addr & ~(CACHE_LINES - 1);
    idx = CacheGetEmpty(&c->cache, bus); 
    
    //Broadcast BusRd on snoop bus
    bus->bus_rd = true;
    bus->bus_rdx = false;
    bus->snoop_addr = addr;
    bus->snoop_core_id = c->core_id;
    bus->snoop_hit = false;
    memset(bus->snoop_data, 0, sizeof(bus->snoop_data));

    //Debug if block
    if (c->core_id == 321 && addr == 0x300) {
        printf("!!! CORE 321 CacheLoad 0x300: data[0]=%d, snoop_hit=%d\n",
               c->cache.lines[idx].data[0], bus->snoop_hit);
    }

    //`chg
    CacheSnoopOtherCores(all_c, num_c, c->core_id, addr, false, bus);
    
    c->cache.lines[idx].address = line_base;
    if(bus->snoop_hit){
        //Another core supplied the data
        for(int w = 0; w < CACHE_LINE_DATA_WORDS; w++){
            c->cache.lines[idx].data[w] = bus->snoop_data[w]; 
        }
        c->cache.lines[idx].state = MESI_SHARED;
    }else{
        //No one has it - fetch from main memory
        for(int w = 0; w < CACHE_LINE_DATA_WORDS; w++){
            c->cache.lines[idx].data[w] =
                *(uint32_t *)(&bus->mem[line_base + (w * 4)]);
        }     
        c->cache.lines[idx].state = MESI_EXCLUSIVE; //this core only has it
    }

    /* return idx; */
    //`chg
    int word_offset = (addr & (CACHE_LINES -1)) / 4;
    return c->cache.lines[idx].data[word_offset];
}

void CacheWriteDirect(CORE *c, uint32_t addr, uint32_t value,
                      BUS *bus, CORE *all_c, int num_c){
    //Debug block
    if (c->core_id == 321 && addr == 0x300) {
        printf("!!! CORE 321 CacheWriteDirect 0x300: value=%d\n", value);
        // print backtrace
    }
    
    int idx = CacheFind(&c->cache, addr);
    if(idx == -1){
        CacheLoad(c, addr, bus, all_c, num_c);
        idx = CacheFind(&c->cache, addr);
    }
    CACHE_LINE *line = &c->cache.lines[idx];
    if(line->state == MESI_SHARED){
        printf("CORE %d: BusRdX broadcast for addr=0x%X (was SHARED)\n",
               c->core_id, addr);
        //mUST invalidate other copies first
        bus->bus_rdx = true;
        bus->bus_rd = false;
        bus->snoop_addr = addr;
        bus->snoop_hit = false;
        bus->snoop_core_id = c->core_id;
        //`chg
        CacheSnoopOtherCores(all_c, num_c, c->core_id, addr, true, bus);
    }
    int word_offset = (addr & (CACHE_LINES - 1)) / 4;
    line->data[word_offset] = value;
    line->state = MESI_MODIFIED;
}

void CacheWrite(CORE *c, uint32_t addr, uint32_t value,
                BUS *bus, CORE *all_c, int num_c){
    (void)bus;
    (void)all_c;
    (void)num_c;
    StoreBufferPush(c, addr, value);
}

void CacheSnoopOtherCores(CORE *all_c, int num_cores, uint32_t core_id,
                          uint32_t addr, bool is_write, BUS *bus){
    for(int i = 0; i < num_cores; i++){
        if(i == (int)core_id) continue;
        CacheSnoop(&all_c[i].cache, addr, is_write, bus);
    }
}


/* This step function's Each Handler (OPCODE) is a mini atomic
   transaction by accident. That's not how silicon work.
   The lock was set and released, but never actually blocked
   anything. It was a costume, not a guard. */
/* void Step(CORE *c, BUS *bus, int core_id){ */
/*     if(c->halted) return; */

/*     if(bus->locked && bus->locked_owner != core_id){ */
/*         return; */
/*     } */
    
/*     INS instr = *(INS *)(&bus->mem[c->pc]); */
/*     c->pc += 4; */

/*     uint8_t opc = (instr >> 24) & 0xff; */

/*     /\* if(opc == LOCK_INC) *\/ */
/*     /\*     bus->locked_owner = core_id; *\/ */
    
/*     if(opc < 14){ */
/*         handlers[opc](c, bus, instr); */
/*     }else{ */
/*         printf("Unkown opcode: %d\n", opc); */
/*         c->halted = false; */
/*     } */
    
/*     /\* if(opc == LOCK_INC) *\/ */
/*     /\*     bus->locked_owner = -1; *\/ */
/* } */

/*
  This step function doesn't need above function it's all in one
  this function replicate the cpu-cycle, where
  one cycle = each core advances by exactly one
  micro-operation. Just like real silicon.
*/
void Step(CORE *c, BUS *bus, CORE *all_c){
    if(c->halted) return;
    
    // CATCH THE BUG
    if(c->core_id == 1 && c->micro_op == STORE_MEM) {
        printf("!!! CORE 1 STORE_MEM: addr=0x%X value=%d\n",
               c->addr, c->regs[c->dest]);
    }
    if(c->core_id == 1 && c->micro_op == CMPXCHG_STORE) {
        printf("!!! CORE 1 CMPXCHG_STORE: addr=0x%X value=%d\n",
               c->addr, c->regs[c->dest]);
    }
    if(c->core_id == 1 && c->micro_op == LOCK_INC_STORE) {
        printf("!!! CORE 1 LOCK_INC_STORE: addr=0x%X\n", c->addr);
    }
    
    if(bus->locked && bus->locked_owner != c->core_id){
        /* printf("Core %d stalled (bus locked)\n", c->core_id); */
        return;
    }

    //    DrainStoreBuffer(c, bus, all_c, NUM_CORES);
    switch(c->micro_op){
        case FETCH: {
            c->current_instr = *(INS *)(&bus->mem[c->pc]);
            if (c->core_id == 1) {
                uint8_t opc = (c->current_instr >> 24) & 0xFF;
                printf("!!! CORE 1 FETCH: pc=0x%X, opcode=%d\n", c->pc, opc);
            }
            c->pc += 4;
            c->micro_op = DECODE;
            break;
        }
        case DECODE: {
            Decode(c->current_instr, &c->opc, &c->dest, &c->src,
                   &c->mode, &c->imm);
            switch(c->opc){
                case LOAD:
                    c->micro_op = LOAD_ADDR;
                    break;
                case STORE:
                    c->micro_op = STORE_ADDR;
                    break;
                case LIC:
                    c->micro_op = LIC_WB;
                    break;
                case ADD:
                    c->micro_op = ADD_EXEC;
                    break;
                case SUB:
                    c->micro_op = SUB_EXEC;
                    break;
                case CMP:
                    c->micro_op = CMP_EXEC;
                    break;
                case JMP:
                    c->micro_op = JMP_EXEC;
                    break;
                case JEQ:
                    c->micro_op = JEQ_EXEC;
                    break;
                case JNE:
                    c->micro_op = JNE_EXEC;
                    break;
                case JLT:
                    c->micro_op = JLT_EXEC;
                    break;
                case JGT:
                    c->micro_op = JGT_EXEC;
                    break;
                case LOCK_INC:
                    c->micro_op = LOCK_INC_ADDR;
                    break;
                case CMPXCHG:
                    c->micro_op = CMPXCHG_ADDR;
                    break;
                case FENCE:
                    c->micro_op = FENCE_EXEC;
                    break;
                case HALT:
                    c->micro_op = HALT_EXEC;
                    break;
            }
            break;
        }
        case LOAD_ADDR: {
            c->addr = (c->mode == 0) ? c->imm : c->regs[c->src];
            c->micro_op = LOAD_MEM;
            break;
        }
        case LOAD_MEM: {
            /* c->temp_val = *(uint32_t *)(&bus->mem[c->addr]); */
            //`chg
            c->temp_val = CacheLoad(c, c->addr, bus, all_c, NUM_CORES);
            if (c->core_id == 321 && c->addr == 0x300) {
                printf("!!! CORE 321: LOAD_MEM for 0x300 got temp_val=%d\n",
                       c->temp_val);
            }
            c->micro_op = LOAD_WB;
            break;
        }
        case LOAD_WB: {
            c->regs[c->dest] = c->temp_val;
            if (c->core_id == 321 && c->dest == R4) {
                printf("!!! CORE 321: LOAD_WB storing %d to R4\n",
                       c->temp_val);
            }
            c->micro_op = FETCH;
            break;
        }
        case STORE_ADDR: {
            c->addr = (c->mode == 0) ? c->imm : c->regs[c->src];
            c->micro_op = STORE_MEM;
            break;
        }
        case STORE_MEM: {
            /* *(uint32_t *)(&bus->mem[c->addr]) = c->regs[c->dest]; */
            //`chg
            CacheWrite(c, c->addr, c->regs[c->dest], bus, all_c, NUM_CORES);
            c->micro_op = FETCH;
            break;
        }
        case LIC_WB: {
            c->regs[c->dest] = c->imm;
            c->micro_op = FETCH;
            break;
        }
        case ADD_EXEC: {
            c->temp_val = c->regs[c->dest] + c->regs[c->src];
            c->micro_op = ADD_WB;
            break;
        }
        case ADD_WB: {
            c->regs[c->dest] = c->temp_val;
            c->micro_op = FETCH;
            break;
        }
        case SUB_EXEC: {
            c->temp_val = c->regs[c->dest] - c->regs[c->src];
            c->micro_op = SUB_WB;
            break;
        }
        case SUB_WB: {
            c->regs[c->dest] = c->temp_val;
            c->micro_op = FETCH;
            break;
        }
        case CMP_EXEC: {
            uint32_t a = c->regs[c->dest];
            uint32_t b = c->regs[c->src];
            c->cmp_result = a - b;
            c->zero_flag = (a == b);
            c->sign_flag = (a < b);
            c->micro_op = FETCH;
            break;
        }
        case JMP_EXEC: {
            c->pc = (c->mode == 0) ? c->imm : c->regs[c->src];
            c->micro_op = FETCH;
            break;
        }
        case JEQ_EXEC: {
            if(c->zero_flag){
                c->pc = (c->mode == 0) ? c->imm : c->regs[c->src];
            }
            c->micro_op = FETCH;
            break;
        }
        case JNE_EXEC: {
            if(!c->zero_flag){
                c->pc = (c->mode == 0) ? c->imm : c->regs[c->src];
            }
            c->micro_op = FETCH;
            break;
        }
        case JLT_EXEC: {
            if(c->sign_flag){
                c->pc = (c->mode == 0) ? c->imm : c->regs[c->src];
            }
            c->micro_op = FETCH;
            break;
        }
        case JGT_EXEC: {
            if(!c->zero_flag && !c->sign_flag){
                c->pc = (c->mode == 0) ? c->imm : c->regs[c->src];
            }
            c->micro_op = FETCH;
            break;
        }
        case LOCK_INC_ADDR: {
            bus->locked = true;
            bus->locked_owner = c->core_id;
            c->addr = (c->mode == 0) ? c->imm : c->regs[c->src];
            c->micro_op = LOCK_INC_LOAD;
            break;
        }
        case LOCK_INC_LOAD: {
            /* c->temp_val = *(uint32_t *)(&bus->mem[c->addr]); */
            //`chg
            c->temp_val = CacheLoad(c, c->addr, bus, all_c, NUM_CORES);
            c->micro_op = LOCK_INC_STORE;
            break;
        }
        case LOCK_INC_STORE: {
            c->temp_val++;
            /* *(uint32_t *)(&bus->mem[c->addr]) = c->temp_val; */
            //`chg
            CacheWrite(c, c->addr, c->temp_val, bus, all_c, NUM_CORES);
            bus->locked = false;
            bus->locked_owner = -1;
            c->micro_op = FETCH;
            break;
        }
        case CMPXCHG_ADDR: {
            bus->locked = true;
            bus->locked_owner = c->core_id;
            c->addr = (c->mode == 0) ? c->imm : c->regs[c->src];
            c->micro_op = CMPXCHG_LOAD;
            break;
        }
        case CMPXCHG_LOAD: {
            /* c->temp_val = *(uint32_t *)(&bus->mem[c->addr]); */
            //`chg
            c->temp_val = CacheLoad(c, c->addr, bus, all_c, NUM_CORES);
            c->micro_op = CMPXCHG_COMPARE;
            break;
        }
        case CMPXCHG_COMPARE: {
            if(c->temp_val == c->regs[EXP]){
                c->micro_op = CMPXCHG_STORE;
            }else{
                c->regs[EXP] = c->temp_val;
                c->zero_flag = 0;
                c->micro_op = CMPXCHG_FAIL;
            }
            break;
        }
        case CMPXCHG_STORE: {
            /* *(uint32_t *)(&bus->mem[c->addr]) = c->regs[c->dest]; */
            //`chg
            CacheWrite(c, c->addr, c->regs[c->dest], bus, all_c, NUM_CORES);
            c->zero_flag = 1;
            bus->locked = false;
            bus->locked_owner = -1;
            c->micro_op = FETCH;
            break;
        }  
        case CMPXCHG_FAIL: {
            bus->locked = false;
            bus->locked_owner = -1;
            c->micro_op = FETCH;
            break;
        }
        case FENCE_EXEC: {
            FlushStoreBuffer(c, bus, all_c, NUM_CORES);
            c->micro_op = FETCH;
            break;
        }
        case HALT_EXEC: {
            c->halted = true;
            break;
        }
        default: {
            printf("Unknown mirco_op: %d\n", c->micro_op);
            c->halted = true;
        }
    }
}

void PrintCache(CORE *c){
    printf("Core %d cache:\n", c->core_id);
    for(int i = 0; i < CACHE_LINES; i++){
        if(c->cache.lines[i].state != MESI_INVALID){
            char state_char = 'I';
            if(c->cache.lines[i].state == MESI_EXCLUSIVE){
                state_char = 'E';
            }
            if(c->cache.lines[i].state == MESI_SHARED){
                state_char = 'S';
            }
            if(c->cache.lines[i].state == MESI_MODIFIED){
                state_char = 'M';
            }
            printf(" [%d] addr= 0x%X data=[%d, %d] state=%c\n",
                   i, c->cache.lines[i].address,
                   c->cache.lines[i].data[0],
                   c->cache.lines[i].data[1],
                   state_char);
        }
    }
}

int main(){
    BUS bus = {0};
    CORE cores[NUM_CORES] = {0};
    cores[0].core_id = 0;
    cores[1].core_id = 1;

    // Core 0: writes data=42 to 0x300, then flag=1 to 0x308
    INS program_core0[] = {
        Encode(LIC,  R0, 0, 0, 0x300),
        Encode(LIC,  R1, 0, 0, 0x308),
        Encode(LIC,  R2, 0, 0, 42),
        Encode(LIC,  R3, 0, 0, 1),
        Encode(STORE, R2, R0, 1, 0),     // [0x300] = 42
        Encode(FENCE, 0, 0, 0, 0),
        Encode(STORE, R3, R1, 1, 0),     // [0x308] = 1
        Encode(FENCE, 0, 0, 0, 0),
        Encode(HALT, 0, 0, 0, 0),
    };
    // Core 1: spins until flag==1, then reads data
    INS program_core1[] = {
        Encode(LIC,  R0, 0, 0, 0x308),   // flag addr
        Encode(LIC,  R1, 0, 0, 0x300),   // data addr
        Encode(LIC,  R2, 0, 0, 0),
        // loop start = 0x10C
        Encode(LOAD,  R3, R0, 1, 0),     // load flag
        Encode(CMP,   R3, R2, 0, 0),
        Encode(JEQ,   0, 0, 0, 0x10C),   // <- FIX: jump to 0x10C, not 0x0C!
        Encode(LOAD,  R4, R1, 1, 0),     // load data
        Encode(HALT, 0, 0, 0, 0),
    };

    memcpy(&bus.mem[0x000], program_core0, sizeof(program_core0));
    *(uint32_t*)&bus.mem[0x300] = 0;
    *(uint32_t*)&bus.mem[0x308] = 0;
    cores[0].pc = 0x000;

    memcpy(&bus.mem[0x100], program_core1, sizeof(program_core1));
    cores[1].pc = 0x100; 
    
    int cycle = 0;
    while(!cores[0].halted || !cores[1].halted){
        Step(&cores[0], &bus, cores);
        Step(&cores[1], &bus, cores);
        cycle++;
    }

    // Flush before reading
    for(int i = 0; i < NUM_CORES; i++){
        FlushStoreBuffer(&cores[i], &bus, cores, NUM_CORES);
    }
    for(int i = 0; i < NUM_CORES; i++){
        for(int j = 0; j < CACHE_LINES; j++){
            if(cores[i].cache.lines[j].state == MESI_MODIFIED){
                uint32_t base = cores[i].cache.lines[j].address;
                for(int w = 0; w < CACHE_LINE_DATA_WORDS; w++){
                    *(uint32_t*)&bus.mem[base + w*4] = cores[i].cache.lines[j].data[w];
                }
            }
        }
    }

    printf("R3 (flag) = %d\n", cores[1].regs[R3]);
    printf("R4 (data) = %d (expected 42)\n", cores[1].regs[R4]);
    printf("Cycles: %d\n", cycle);
    return 0;
}
