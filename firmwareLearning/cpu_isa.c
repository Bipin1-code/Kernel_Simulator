
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <string.h>
#include <stdbool.h>

#define EXP R15 //Dedicated Expected value register

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
    HALT_EXEC
} MICRO_OP; 

typedef enum _OPCODE{
    LOAD = 0, STORE, LIC, ADD, SUB,
    CMP, JMP, JEQ, JNE, JLT, JGT,
    LOCK_INC, CMPXCHG,
    HALT
} OPCODE;

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
} CORE;

typedef struct _BUS{
    uint8_t mem[1024];
    bool locked;
    int locked_owner;
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
    [HALT]     = HaltFunction
};

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

/*This step function doesn't need above function it's all in one
  this function replicate the cpu-cycle, where
  one cycle = each core advances by exactly one
  micro-operation. Just like real silicon */
void Step(CORE *c, BUS *bus, int core_id){
    if(c->halted) return;
    
    if(bus->locked && bus->locked_owner != core_id){
        printf("Core %d stalled (bus locked)\n", core_id);
        return;
    }
    switch(c->micro_op){
        case FETCH: {
            c->current_instr = *(INS *)(&bus->mem[c->pc]);
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
            c->temp_val = *(uint32_t *)(&bus->mem[c->addr]);
            c->micro_op = LOAD_WB;
            break;
        }
        case LOAD_WB: {
            c->regs[c->dest] = c->temp_val;
            c->micro_op = FETCH;
            break;
        }
        case STORE_ADDR: {
            c->addr = (c->mode == 0) ? c->imm : c->regs[c->src];
            c->micro_op = STORE_MEM;
            break;
        }
        case STORE_MEM: {
            *(uint32_t *)(&bus->mem[c->addr]) = c->regs[c->dest];
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
            bus->locked_owner = core_id;
            c->addr = (c->mode == 0) ? c->imm : c->regs[c->src];
            c->micro_op = LOCK_INC_LOAD;
            break;
        }
        case LOCK_INC_LOAD: {
            c->temp_val = *(uint32_t *)(&bus->mem[c->addr]);
            c->micro_op = LOCK_INC_STORE;
            break;
        }
        case LOCK_INC_STORE: {
            c->temp_val++;
            *(uint32_t *)(&bus->mem[c->addr]) = c->temp_val;
            bus->locked = false;
            bus->locked_owner = -1;
            c->micro_op = FETCH;
            break;
        }
        case CMPXCHG_ADDR: {
            bus->locked = true;
            bus->locked_owner = core_id;
            c->addr = (c->mode == 0) ? c->imm : c->regs[c->src];
            c->micro_op = CMPXCHG_LOAD;
            break;
        }
        case CMPXCHG_LOAD: {
            c->temp_val = *(uint32_t *)(&bus->mem[c->addr]);
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
            *(uint32_t *)(&bus->mem[c->addr]) = c->regs[c->dest];
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

int main(){
    BUS bus = {0};
    CORE core0 = {0};
    core0.core_id = 123;
    CORE core1 = {0};
    core1.core_id = 321;
    
    INS program[] = {
        // R0 = address of counter (0x300)
        Encode(LIC,  R0,  0, 0, 0x300),

        // R1 = loop counter (10)
        Encode(LIC,  R1,  0, 0, 10),

        // R2 = constant 1
        Encode(LIC,  R2,  0, 0, 1),

        // R3 = constant 0 (for CMP)
        Encode(LIC,  R3,  0, 0, 0),

        // --- loop start (0x10) ---
        // Load current counter into EXP (expected)
        Encode(LOAD,  EXP, R0, 1, 0),        // EXP = mem[counter]

        // --- retry point (0x14) ---

        // R4 = EXP + 1 (new value)
        Encode(LIC,   R4, 0, 0, 0),          // clear R4
        Encode(ADD,   R4, EXP, 0, 0),        // R4 = EXP
        Encode(ADD,   R4, R2, 0, 0),         // R4 = EXP + 1

        // CMPXCHG [R0], R4
        Encode(CMPXCHG, R4, R0, 1, 0),       // if mem[R0]==EXP -> mem[R0]=R4, ZF=1
        // else EXP = mem[R0], ZF=0

        // If failed, retry
        Encode(JNE,  0, 0, 0, 0x14),         // jump back to retry point

        // Success: decrement loop counter
        Encode(SUB,   R1, R2, 0, 0),         // R1--
        Encode(CMP,   R1, R3, 0, 0),         // R1 == 0?
        Encode(JNE,   0, 0, 0, 0x10),        // back to loop start

        
        Encode(HALT,  0, 0, 0, 0),
    };

    memcpy(bus.mem, program, sizeof(program));
    *(uint32_t *)(&bus.mem[0x300]) = 0;

    int cycle = 0;
    while(!core0.halted || !core1.halted){
        Step(&core0, &bus, 123);
        Step(&core1, &bus, 321);
        cycle++;
    }
    printf("Total cycles: %d\n", cycle);
    printf("Counter = %d\n", *(uint32_t*)&bus.mem[0x300]);
    //print expected and outcomes here
    printf("EXP = %d, R0 = 0x%X, R1 = %d, R2 = %d, R3 = %d, R4 = %d\n",
           core0.regs[EXP], core0.regs[R0],
           core0.regs[R1], core0.regs[R2],
           core0.regs[R3], core0.regs[R4]);
    
    return 0;
}
 
