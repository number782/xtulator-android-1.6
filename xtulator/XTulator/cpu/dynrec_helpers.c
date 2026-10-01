/*
    Dynarec helper implementations for x86 -> ARM translation.

    Each helper function mirrors the logic in cpu_exec() inside cpu.c,
    but operates on the actual CPU_t state. These are called from
    generated ARM code via BL instructions.

    Calling convention (AAPCS):
      R0 = CPU_t* (also available in R4 as callee-saved)
      R1 = argument (regnum, immediate value, etc.)
      Return value in R0: 0 = continue block, 1 = break out of block

    Note: These helpers deliberately duplicate the logic from cpu.c
    to avoid dependencies on its internal inline functions. The FUNC_INLINE
    functions in cpu.c are always inlined and not available for linking.
*/

#include <stdint.h>
#include "cpu.h"
#include "../memory.h"
#include "../chipset/i8259.h"

extern const uint8_t byteregtable[8];

/* ---- Simple opcodes ---- */

int dinstr_nop(CPU_t* cpu) {
    (void)cpu;
    return 0;  /* NOP does nothing, always continue */
}

int dinstr_hlt(CPU_t* cpu) {
    cpu->hltstate = 1;
    return 0;
}

/* Execute one instruction via the interpreter — used for opcodes not
   natively translated by the dynrec (ModR/M instructions, etc.)
   Returns the number of bytes the instruction consumed (1-3). */
int dinstr_exec_one(CPU_t* cpu) {
    uint16_t ip_before = cpu->ip;
    cpu_exec(cpu, 1);
    return cpu->ip - ip_before;
}

/* ---- Register 16-bit operations ---- */

int dinstr_inc_r16(CPU_t* cpu, uint8_t regnum) {
    uint16_t val = cpu->regs.wordregs[regnum];
    uint16_t res = val + 1;
    cpu->cf = cpu->oldcf;  /* INC preserves CF */
    
    uint16_t tmp = (uint16_t)(val + 1);
    if (!tmp) cpu->zf = 1; else cpu->zf = 0;
    if (tmp & 0x8000) cpu->sf = 1; else cpu->sf = 0;
    /* AF: set if low nibble overflow */
    if (((val & 0x0F) + 1) > 0x0F) cpu->af = 1; else cpu->af = 0;
    /* OF for INC: set if result is 0x8000 (overflow from 0x7FFF) */
    if (val == 0x7FFF) cpu->of = 1; else cpu->of = 0;
    
    cpu->regs.wordregs[regnum] = res;
    return 0;
}

int dinstr_dec_r16(CPU_t* cpu, uint8_t regnum) {
    uint16_t val = cpu->regs.wordregs[regnum];
    uint16_t res = val - 1;
    cpu->cf = cpu->oldcf;  /* DEC preserves CF */
    
    uint16_t tmp = (uint16_t)(val - 1);
    if (!tmp) cpu->zf = 1; else cpu->zf = 0;
    if (tmp & 0x8000) cpu->sf = 1; else cpu->sf = 0;
    if ((val & 0x0F) < 1) cpu->af = 1; else cpu->af = 0;
    if (val == 0x8000) cpu->of = 1; else cpu->of = 0;
    
    cpu->regs.wordregs[regnum] = res;
    return 0;
}

int dinstr_push_r16(CPU_t* cpu, uint8_t regnum) {
    cpu->regs.wordregs[regsp] -= 2;
    putmem16(cpu, cpu->segregs[regss], cpu->regs.wordregs[regsp], cpu->regs.wordregs[regnum]);
    return 0;
}

int dinstr_pop_r16(CPU_t* cpu, uint8_t regnum) {
    cpu->regs.wordregs[regnum] = getmem16(cpu, cpu->segregs[regss], cpu->regs.wordregs[regsp]);
    cpu->regs.wordregs[regsp] += 2;
    return 0;
}

/* ---- Data movement ---- */

int dinstr_mov_r_i(CPU_t* cpu, uint8_t regnum, uint16_t imm) {
    cpu->regs.wordregs[regnum] = imm;
    return 0;
}

/* ---- Control flow ---- */

int dinstr_jmp_rel8(CPU_t* cpu, int8_t rel) {
    cpu->ip += 2;
    cpu->ip += rel;
    /* JMP terminates the basic block */
    return 1;
}

int dinstr_jcond_rel8(CPU_t* cpu, uint8_t opcode, int8_t rel) {
    int taken = 0;

    switch (opcode) {
    case 0x70: taken = cpu->of;                         break;  /* JO  */
    case 0x71: taken = !cpu->of;                        break;  /* JNO */
    case 0x72: taken = cpu->cf;                         break;  /* JB  */
    case 0x73: taken = !cpu->cf;                        break;  /* JNB */
    case 0x74: taken = cpu->zf;                         break;  /* JZ  */
    case 0x75: taken = !cpu->zf;                        break;  /* JNZ */
    case 0x76: taken = cpu->cf || cpu->zf;              break;  /* JBE */
    case 0x77: taken = !cpu->cf && !cpu->zf;            break;  /* JA  */
    case 0x78: taken = cpu->sf;                         break;  /* JS  */
    case 0x79: taken = !cpu->sf;                        break;  /* JNS */
    case 0x7A: taken = cpu->pf;                         break;  /* JPE */
    case 0x7B: taken = !cpu->pf;                        break;  /* JPO */
    case 0x7C: taken = (cpu->sf != cpu->of);           break;  /* JL  */
    case 0x7D: taken = (cpu->sf == cpu->of);            break;  /* JGE */
    case 0x7E: taken = (cpu->sf != cpu->of) || cpu->zf; break;  /* JLE */
    case 0x7F: taken = !cpu->zf && (cpu->sf == cpu->of);break;  /* JG  */
    }

    cpu->ip += 2;  /* Always advance past the 2-byte instruction */
    if (taken) {
        cpu->ip += rel;
    }

    return 0;  /* Continue the block — execution flows sequentially if not taken */
}

int dinstr_jmp_rel16(CPU_t* cpu, int16_t rel) {
    cpu->ip += 3;
    cpu->ip += rel;
    /* JMP terminates the basic block */
    return 1;
}

int dinstr_call_rel16(CPU_t* cpu, int16_t rel) {
    cpu->regs.wordregs[regsp] -= 2;
    putmem16(cpu, cpu->segregs[regss], cpu->regs.wordregs[regsp], cpu->ip + 3);
    cpu->ip += 3 + rel;
    /* CALL terminates the basic block */
    return 1;
}

int dinstr_ret_near(CPU_t* cpu) {
    cpu->ip = getmem16(cpu, cpu->segregs[regss], cpu->regs.wordregs[regsp]);
    cpu->regs.wordregs[regsp] += 2;
    /* RET terminates the basic block */
    return 1;
}

int dinstr_int(CPU_t* cpu, uint8_t intnum) {
    cpu->ip += 2;  /* Advance past the 2-byte INT instruction before calling */
    cpu_intcall(cpu, intnum);
    /* INT can cause a far jump, terminate the block */
    return 1;
}

/* ---- Flag operations ---- */

int dinstr_pushf(CPU_t* cpu) {
    cpu->regs.wordregs[regsp] -= 2;
    putmem16(cpu, cpu->segregs[regss], cpu->regs.wordregs[regsp], makeflagsword(cpu));
    return 0;
}

int dinstr_popf(CPU_t* cpu) {
    uint16_t flags = getmem16(cpu, cpu->segregs[regss], cpu->regs.wordregs[regsp]);
    cpu->regs.wordregs[regsp] += 2;
    decodeflagsword(cpu, flags);
    return 0;
}

/* ---- INC/DEC r8 ---- */

static uint8_t calc_parity(uint8_t val) {
    val ^= val >> 4;
    val ^= val >> 2;
    val ^= val >> 1;
    return (val & 1) ^ 1;
}

int dinstr_inc_r8(CPU_t* cpu, uint8_t regnum) {
    uint8_t val = cpu->regs.byteregs[regnum & 0x07];
    cpu->oldcf = cpu->cf;
    uint8_t res = val + 1;
    cpu->cf = cpu->oldcf;
    
    uint16_t tmp = (uint16_t)(val + 1);
    if (!tmp) cpu->zf = 1; else cpu->zf = 0;
    if (tmp & 0x80) cpu->sf = 1; else cpu->sf = 0;
    if (((val & 0x0F) + 1) > 0x0F) cpu->af = 1; else cpu->af = 0;
    if (val == 0x7F) cpu->of = 1; else cpu->of = 0;
    cpu->pf = calc_parity(tmp);
    
    cpu->regs.byteregs[regnum & 0x07] = res;
    return 0;
}

int dinstr_dec_r8(CPU_t* cpu, uint8_t regnum) {
    uint8_t val = cpu->regs.byteregs[regnum & 0x07];
    cpu->oldcf = cpu->cf;
    uint8_t res = val - 1;
    cpu->cf = cpu->oldcf;
    
    uint16_t tmp = (uint16_t)(val - 1);
    if (!tmp) cpu->zf = 1; else cpu->zf = 0;
    if (tmp & 0x80) cpu->sf = 1; else cpu->sf = 0;
    if ((val & 0x0F) < 1) cpu->af = 1; else cpu->af = 0;
    if (val == 0x80) cpu->of = 1; else cpu->of = 0;
    cpu->pf = calc_parity(tmp);
    
    cpu->regs.byteregs[regnum & 0x07] = res;
    return 0;
}

/* ---- Arithmetic with immediate ---- */

int dinstr_add_r16_imm(CPU_t* cpu, uint8_t regnum, uint16_t imm) {
    uint16_t v1 = cpu->regs.wordregs[regnum];
    uint32_t dst = (uint32_t)v1 + (uint32_t)imm;
    
    cpu->res16 = (uint16_t)dst;
    if (!cpu->res16) cpu->zf = 1; else cpu->zf = 0;
    if (cpu->res16 & 0x8000) cpu->sf = 1; else cpu->sf = 0;
    cpu->pf = calc_parity(cpu->res16 & 0xFF);
    if (dst & 0xFFFF0000) cpu->cf = 1; else cpu->cf = 0;
    if (((dst ^ v1) & (dst ^ imm) & 0x8000) == 0x8000) cpu->of = 1; else cpu->of = 0;
    if (((v1 ^ imm ^ dst) & 0x10) == 0x10) cpu->af = 1; else cpu->af = 0;
    
    cpu->regs.wordregs[regnum] = cpu->res16;
    return 0;
}

/* 0x04: ADD AL, Ib — Add immediate8 to AL, set all flags */
int dinstr_add_al_imm8(CPU_t* cpu, uint8_t imm) {
    uint8_t v1 = cpu->regs.byteregs[regal];
    uint16_t dst = (uint16_t)v1 + (uint16_t)imm;

    cpu->res8 = (uint8_t)dst;
    if (!cpu->res8) cpu->zf = 1; else cpu->zf = 0;
    if (cpu->res8 & 0x80) cpu->sf = 1; else cpu->sf = 0;
    cpu->pf = calc_parity(cpu->res8);
    if (dst & 0xFF00) cpu->cf = 1; else cpu->cf = 0;
    if (((dst ^ v1) & (dst ^ imm) & 0x80) == 0x80) cpu->of = 1; else cpu->of = 0;
    if (((v1 ^ imm ^ dst) & 0x10) == 0x10) cpu->af = 1; else cpu->af = 0;

    cpu->regs.byteregs[regal] = cpu->res8;
    return 0;
}

/* 0x05: ADD AX, Iv — Add immediate16 to AX, set all flags */
int dinstr_add_ax_imm16(CPU_t* cpu, uint16_t imm) {
    uint16_t v1 = cpu->regs.wordregs[regax];
    uint32_t dst = (uint32_t)v1 + (uint32_t)imm;

    cpu->res16 = (uint16_t)dst;
    if (!cpu->res16) cpu->zf = 1; else cpu->zf = 0;
    if (cpu->res16 & 0x8000) cpu->sf = 1; else cpu->sf = 0;
    cpu->pf = calc_parity(cpu->res16 & 0xFF);
    if (dst & 0xFFFF0000) cpu->cf = 1; else cpu->cf = 0;
    if (((dst ^ v1) & (dst ^ imm) & 0x8000) == 0x8000) cpu->of = 1; else cpu->of = 0;
    if (((v1 ^ imm ^ dst) & 0x10) == 0x10) cpu->af = 1; else cpu->af = 0;

    cpu->regs.wordregs[regax] = cpu->res16;
    return 0;
}

int dinstr_cmp_r16_imm(CPU_t* cpu, uint8_t regnum, uint16_t imm) {
    uint16_t v1 = cpu->regs.wordregs[regnum];
    uint32_t dst = (uint32_t)v1 - (uint32_t)imm;
    
    cpu->res16 = (uint16_t)dst;
    if (!cpu->res16) cpu->zf = 1; else cpu->zf = 0;
    if (cpu->res16 & 0x8000) cpu->sf = 1; else cpu->sf = 0;
    cpu->pf = calc_parity(cpu->res16 & 0xFF);
    if (dst & 0xFFFF0000) cpu->cf = 1; else cpu->cf = 0;
    if ((dst ^ v1) & (v1 ^ imm) & 0x8000) cpu->of = 1; else cpu->of = 0;
    if ((v1 ^ imm ^ dst) & 0x10) cpu->af = 1; else cpu->af = 0;
    
    return 0;
}

/* ---- Segment register push/pop ---- */

int dinstr_push_es(CPU_t* cpu) {
    cpu->regs.wordregs[regsp] -= 2;
    putmem16(cpu, cpu->segregs[regss], cpu->regs.wordregs[regsp], cpu->segregs[reges]);
    return 0;
}

int dinstr_pop_es(CPU_t* cpu) {
    cpu->segregs[reges] = getmem16(cpu, cpu->segregs[regss], cpu->regs.wordregs[regsp]);
    cpu->regs.wordregs[regsp] += 2;
    return 0;
}

int dinstr_push_cs(CPU_t* cpu) {
    cpu->regs.wordregs[regsp] -= 2;
    putmem16(cpu, cpu->segregs[regss], cpu->regs.wordregs[regsp], cpu->segregs[regcs]);
    return 0;
}

int dinstr_pop_cs(CPU_t* cpu) {
    cpu->segregs[regcs] = getmem16(cpu, cpu->segregs[regss], cpu->regs.wordregs[regsp]);
    cpu->regs.wordregs[regsp] += 2;
    return 0;
}

int dinstr_push_ss(CPU_t* cpu) {
    cpu->regs.wordregs[regsp] -= 2;
    putmem16(cpu, cpu->segregs[regss], cpu->regs.wordregs[regsp], cpu->segregs[regss]);
    return 0;
}

int dinstr_pop_ss(CPU_t* cpu) {
    cpu->segregs[regss] = getmem16(cpu, cpu->segregs[regss], cpu->regs.wordregs[regsp]);
    cpu->regs.wordregs[regsp] += 2;
    return 0;
}

int dinstr_push_ds(CPU_t* cpu) {
    cpu->regs.wordregs[regsp] -= 2;
    putmem16(cpu, cpu->segregs[regss], cpu->regs.wordregs[regsp], cpu->segregs[regds]);
    return 0;
}

int dinstr_pop_ds(CPU_t* cpu) {
    cpu->segregs[regds] = getmem16(cpu, cpu->segregs[regss], cpu->regs.wordregs[regsp]);
    cpu->regs.wordregs[regsp] += 2;
    return 0;
}

/* ---- Flag operations ---- */

int dinstr_cmc(CPU_t* cpu) {
    cpu->cf = !cpu->cf;
    return 0;
}

int dinstr_clc(CPU_t* cpu) {
    cpu->cf = 0;
    return 0;
}

int dinstr_stc(CPU_t* cpu) {
    cpu->cf = 1;
    return 0;
}

int dinstr_cli(CPU_t* cpu) {
    cpu->ifl = 0;
    return 0;
}

int dinstr_sti(CPU_t* cpu) {
    cpu->ifl = 1;
    return 0;
}

int dinstr_cld(CPU_t* cpu) {
    cpu->df = 0;
    return 0;
}

int dinstr_std(CPU_t* cpu) {
    cpu->df = 1;
    return 0;
}

/* ---- SAHF/LAHF ---- */

int dinstr_sahf(CPU_t* cpu) {
    uint8_t al = cpu->regs.byteregs[regal];
    /* SAHF loads lower 5 bits of flags from AH */
    cpu->cf = al & 1;
    cpu->pf = (al >> 2) & 1;
    cpu->af = (al >> 4) & 1;
    cpu->zf = (al >> 6) & 1;
    cpu->sf = (al >> 7) & 1;
    return 0;
}

int dinstr_lahf(CPU_t* cpu) {
    cpu->regs.byteregs[regah] = makeflagsword(cpu) & 0xFF;
    return 0;
}

/* ---- ModR/M-based instruction helpers ---- */
/* These replicate the FUNC_INLINE logic from cpu.c, operating on CPU_t state. */

static void dynrec_getea(CPU_t* cpu, uint8_t rmval) {
    uint32_t tempea = 0;
    switch (cpu->mode) {
    case 0:
        switch (rmval) {
        case 0: tempea = cpu->regs.wordregs[regbx] + cpu->regs.wordregs[regsi]; break;
        case 1: tempea = cpu->regs.wordregs[regbx] + cpu->regs.wordregs[regdi]; break;
        case 2: tempea = cpu->regs.wordregs[regbp] + cpu->regs.wordregs[regsi]; break;
        case 3: tempea = cpu->regs.wordregs[regbp] + cpu->regs.wordregs[regdi]; break;
        case 4: tempea = cpu->regs.wordregs[regsi]; break;
        case 5: tempea = cpu->regs.wordregs[regdi]; break;
        case 6: tempea = cpu->disp16; break;
        case 7: tempea = cpu->regs.wordregs[regbx]; break;
        }
        break;
    case 1:
    case 2:
        switch (rmval) {
        case 0: tempea = cpu->regs.wordregs[regbx] + cpu->regs.wordregs[regsi] + cpu->disp16; break;
        case 1: tempea = cpu->regs.wordregs[regbx] + cpu->regs.wordregs[regdi] + cpu->disp16; break;
        case 2: tempea = cpu->regs.wordregs[regbp] + cpu->regs.wordregs[regsi] + cpu->disp16; break;
        case 3: tempea = cpu->regs.wordregs[regbp] + cpu->regs.wordregs[regdi] + cpu->disp16; break;
        case 4: tempea = cpu->regs.wordregs[regsi] + cpu->disp16; break;
        case 5: tempea = cpu->regs.wordregs[regdi] + cpu->disp16; break;
        case 6: tempea = cpu->regs.wordregs[regbp] + cpu->disp16; break;
        case 7: tempea = cpu->regs.wordregs[regbx] + cpu->disp16; break;
        }
        break;
    default:
        break;
    }
    cpu->ea = (uint32_t)(tempea & 0xFFFF) + (cpu->useseg << 4);
}

static uint8_t dynrec_readrm8(CPU_t* cpu, uint8_t rmval) {
    if (cpu->mode < 3) {
        dynrec_getea(cpu, rmval);
        return cpu_read(cpu, cpu->ea);
    }
    return cpu->regs.byteregs[byteregtable[rmval]];
}

static uint16_t dynrec_readrm16(CPU_t* cpu, uint8_t rmval) {
    if (cpu->mode < 3) {
        dynrec_getea(cpu, rmval);
        return cpu_read(cpu, cpu->ea) | ((uint16_t)cpu_read(cpu, cpu->ea + 1) << 8);
    }
    return cpu->regs.wordregs[rmval];
}

static void dynrec_writerm8(CPU_t* cpu, uint8_t rmval, uint8_t value) {
    if (cpu->mode < 3) {
        dynrec_getea(cpu, rmval);
        cpu_write(cpu, cpu->ea, value);
    } else {
        cpu->regs.byteregs[byteregtable[rmval]] = value;
    }
}

static void dynrec_writerm16(CPU_t* cpu, uint8_t rmval, uint16_t value) {
    if (cpu->mode < 3) {
        dynrec_getea(cpu, rmval);
        cpu_write(cpu, cpu->ea, value & 0xFF);
        cpu_write(cpu, cpu->ea + 1, value >> 8);
    } else {
        cpu->regs.wordregs[rmval] = value;
    }
}

/* 0x84: TEST Gb, Eb — AND operands, set flags, discard result (8-bit) */
int dinstr_test_gb_eb(CPU_t* cpu) {
    cpu->segoverride = 0;
    cpu->useseg = cpu->segregs[regds];
    modregrm(cpu);

    uint8_t oper1 = cpu->regs.byteregs[byteregtable[cpu->reg]];
    uint8_t oper2 = dynrec_readrm8(cpu, cpu->rm);

    uint8_t res = oper1 & oper2;
    if (!res) cpu->zf = 1; else cpu->zf = 0;
    if (res & 0x80) cpu->sf = 1; else cpu->sf = 0;
    cpu->pf = calc_parity(res);
    cpu->cf = 0;
    cpu->of = 0;

    return 0;
}

/* 0x85: TEST Gv, Ev — AND operands, set flags, discard result (16-bit) */
int dinstr_test_gv_ev(CPU_t* cpu) {
    cpu->segoverride = 0;
    cpu->useseg = cpu->segregs[regds];
    modregrm(cpu);

    uint16_t oper1 = cpu->regs.wordregs[cpu->reg];
    uint16_t oper2 = dynrec_readrm16(cpu, cpu->rm);

    uint16_t res = oper1 & oper2;
    if (!res) cpu->zf = 1; else cpu->zf = 0;
    if (res & 0x8000) cpu->sf = 1; else cpu->sf = 0;
    cpu->pf = calc_parity(res & 0xFF);
    cpu->cf = 0;
    cpu->of = 0;

    return 0;
}

/* 0x8C: MOV Ew, Sw — Store segment register to r/m16 */
int dinstr_mov_ew_sw(CPU_t* cpu) {
    cpu->segoverride = 0;
    cpu->useseg = cpu->segregs[regds];
    modregrm(cpu);

    uint16_t value = cpu->segregs[cpu->reg];
    dynrec_writerm16(cpu, cpu->rm, value);

    return 0;
}

/* 0x8D: LEA Gv, M — Load effective address into register */
int dinstr_lea_gv_m(CPU_t* cpu) {
    cpu->segoverride = 0;
    cpu->useseg = cpu->segregs[regds];
    modregrm(cpu);

    dynrec_getea(cpu, cpu->rm);
    cpu->regs.wordregs[cpu->reg] = cpu->ea - (cpu->useseg << 4);

    return 0;
}

/* 0x8E: MOV Sw, Ew — Load segment register from r/m16 */
int dinstr_mov_sw_ew(CPU_t* cpu) {
    cpu->segoverride = 0;
    cpu->useseg = cpu->segregs[regds];
    modregrm(cpu);

    uint16_t value = dynrec_readrm16(cpu, cpu->rm);
    cpu->segregs[cpu->reg] = value;

    return 0;
}

/* 0x0C: OR AL, Ib — OR AL with immediate8, store in AL */
int dinstr_or_al_imm8(CPU_t* cpu, uint8_t imm) {
    uint8_t res = cpu->regs.byteregs[regal] | imm;
    if (!res) cpu->zf = 1; else cpu->zf = 0;
    if (res & 0x80) cpu->sf = 1; else cpu->sf = 0;
    cpu->pf = calc_parity(res);
    cpu->cf = 0;
    cpu->of = 0;
    cpu->regs.byteregs[regal] = res;
    return 0;
}

/* 0x0D: OR AX, Iv — OR AX with immediate16, store in AX */
int dinstr_or_ax_imm16(CPU_t* cpu, uint16_t imm) {
    uint16_t res = cpu->regs.wordregs[regax] | imm;
    if (!res) cpu->zf = 1; else cpu->zf = 0;
    if (res & 0x8000) cpu->sf = 1; else cpu->sf = 0;
    cpu->pf = calc_parity(res & 0xFF);
    cpu->cf = 0;
    cpu->of = 0;
    cpu->regs.wordregs[regax] = res;
    return 0;
}

/* 0x34: XOR AL, Ib — XOR AL with immediate8, store in AL */
int dinstr_xor_al_imm8(CPU_t* cpu, uint8_t imm) {
    uint8_t res = cpu->regs.byteregs[regal] ^ imm;
    if (!res) cpu->zf = 1; else cpu->zf = 0;
    if (res & 0x80) cpu->sf = 1; else cpu->sf = 0;
    cpu->pf = calc_parity(res);
    cpu->cf = 0;
    cpu->of = 0;
    cpu->regs.byteregs[regal] = res;
    return 0;
}

/* 0x35: XOR AX, Iv — XOR AX with immediate16, store in AX */
int dinstr_xor_ax_imm16(CPU_t* cpu, uint16_t imm) {
    uint16_t res = cpu->regs.wordregs[regax] ^ imm;
    if (!res) cpu->zf = 1; else cpu->zf = 0;
    if (res & 0x8000) cpu->sf = 1; else cpu->sf = 0;
    cpu->pf = calc_parity(res & 0xFF);
    cpu->cf = 0;
    cpu->of = 0;
    cpu->regs.wordregs[regax] = res;
    return 0;
}

/* ---- SUB immediate ---- */

/* 0x2C: SUB AL, Ib — Subtract immediate8 from AL, set all flags */
int dinstr_sub_al_imm8(CPU_t* cpu, uint8_t imm) {
    uint8_t v1 = cpu->regs.byteregs[regal];
    uint16_t dst = (uint16_t)v1 - (uint16_t)imm;

    cpu->res8 = (uint8_t)dst;
    if (!cpu->res8) cpu->zf = 1; else cpu->zf = 0;
    if (cpu->res8 & 0x80) cpu->sf = 1; else cpu->sf = 0;
    cpu->pf = calc_parity(cpu->res8);
    if (dst & 0xFF00) cpu->cf = 1; else cpu->cf = 0;
    if (((v1 ^ imm ^ dst) & 0x80) == 0x80) cpu->of = 1; else cpu->of = 0;
    if (((v1 ^ imm ^ dst) & 0x10) == 0x10) cpu->af = 1; else cpu->af = 0;

    cpu->regs.byteregs[regal] = cpu->res8;
    return 0;
}

/* 0x2D: SUB AX, Iv — Subtract immediate16 from AX, set all flags */
int dinstr_sub_ax_imm16(CPU_t* cpu, uint16_t imm) {
    uint16_t v1 = cpu->regs.wordregs[regax];
    uint32_t dst = (uint32_t)v1 - (uint32_t)imm;

    cpu->res16 = (uint16_t)dst;
    if (!cpu->res16) cpu->zf = 1; else cpu->zf = 0;
    if (cpu->res16 & 0x8000) cpu->sf = 1; else cpu->sf = 0;
    cpu->pf = calc_parity(cpu->res16 & 0xFF);
    if (dst & 0xFFFF0000) cpu->cf = 1; else cpu->cf = 0;
    if (((dst ^ v1) & (v1 ^ imm) & 0x8000) == 0x8000) cpu->of = 1; else cpu->of = 0;
    if (((v1 ^ imm ^ dst) & 0x10) == 0x10) cpu->af = 1; else cpu->af = 0;

    cpu->regs.wordregs[regax] = cpu->res16;
    return 0;
}

/* ---- CMP immediate ---- */

/* 0x3C: CMP AL, Ib — Subtract immediate8 from AL, set flags, discard result */
int dinstr_cmp_al_imm8(CPU_t* cpu, uint8_t imm) {
    uint8_t v1 = cpu->regs.byteregs[regal];
    uint16_t dst = (uint16_t)v1 - (uint16_t)imm;

    cpu->res8 = (uint8_t)dst;
    if (!cpu->res8) cpu->zf = 1; else cpu->zf = 0;
    if (cpu->res8 & 0x80) cpu->sf = 1; else cpu->sf = 0;
    cpu->pf = calc_parity(cpu->res8);
    if (dst & 0xFF00) cpu->cf = 1; else cpu->cf = 0;
    if (((v1 ^ imm ^ dst) & 0x80) == 0x80) cpu->of = 1; else cpu->of = 0;
    if (((v1 ^ imm ^ dst) & 0x10) == 0x10) cpu->af = 1; else cpu->af = 0;

    /* CMP discards result — don't write back */
    return 0;
}

/* 0x3D: CMP AX, Iv — Subtract immediate16 from AX, set flags, discard result */
int dinstr_cmp_ax_imm16(CPU_t* cpu, uint16_t imm) {
    uint16_t v1 = cpu->regs.wordregs[regax];
    uint32_t dst = (uint32_t)v1 - (uint32_t)imm;

    cpu->res16 = (uint16_t)dst;
    if (!cpu->res16) cpu->zf = 1; else cpu->zf = 0;
    if (cpu->res16 & 0x8000) cpu->sf = 1; else cpu->sf = 0;
    cpu->pf = calc_parity(cpu->res16 & 0xFF);
    if (dst & 0xFFFF0000) cpu->cf = 1; else cpu->cf = 0;
    if (((dst ^ v1) & (v1 ^ imm) & 0x8000) == 0x8000) cpu->of = 1; else cpu->of = 0;
    if (((v1 ^ imm ^ dst) & 0x10) == 0x10) cpu->af = 1; else cpu->af = 0;

    /* CMP discards result — don't write back */
    return 0;
}

/* 0x14: ADC AL, Ib — Add AL + immediate8 + CF, store in AL */
int dinstr_adc_al_imm8(CPU_t* cpu, uint8_t imm) {
    uint8_t v1 = cpu->regs.byteregs[regal];
    uint16_t dst = (uint16_t)v1 + (uint16_t)imm + cpu->cf;

    cpu->res8 = (uint8_t)dst;
    if (!cpu->res8) cpu->zf = 1; else cpu->zf = 0;
    if (cpu->res8 & 0x80) cpu->sf = 1; else cpu->sf = 0;
    cpu->pf = calc_parity(cpu->res8);
    if (dst & 0xFF00) cpu->cf = 1; else cpu->cf = 0;
    if (((dst ^ v1) & (dst ^ imm) & 0x80) == 0x80) cpu->of = 1; else cpu->of = 0;
    if (((v1 ^ imm ^ dst) & 0x10) == 0x10) cpu->af = 1; else cpu->af = 0;

    cpu->regs.byteregs[regal] = cpu->res8;
    return 0;
}

/* 0x15: ADC AX, Iv — Add AX + immediate16 + CF, store in AX */
int dinstr_adc_ax_imm16(CPU_t* cpu, uint16_t imm) {
    uint16_t v1 = cpu->regs.wordregs[regax];
    uint32_t dst = (uint32_t)v1 + (uint32_t)imm + cpu->cf;

    cpu->res16 = (uint16_t)dst;
    if (!cpu->res16) cpu->zf = 1; else cpu->zf = 0;
    if (cpu->res16 & 0x8000) cpu->sf = 1; else cpu->sf = 0;
    cpu->pf = calc_parity(cpu->res16 & 0xFF);
    if (dst & 0xFFFF0000) cpu->cf = 1; else cpu->cf = 0;
    if (((dst ^ v1) & (dst ^ imm) & 0x8000) == 0x8000) cpu->of = 1; else cpu->of = 0;
    if (((v1 ^ imm ^ dst) & 0x10) == 0x10) cpu->af = 1; else cpu->af = 0;

    cpu->regs.wordregs[regax] = cpu->res16;
    return 0;
}

/* 0x1C: SBB AL, Ib — Subtract immediate8 + CF (borrow) from AL */
int dinstr_sbb_al_imm8(CPU_t* cpu, uint8_t imm) {
    uint8_t v1 = cpu->regs.byteregs[regal];
    uint16_t dst = (uint16_t)v1 - (uint16_t)(imm + cpu->cf);

    cpu->res8 = (uint8_t)dst;
    if (!cpu->res8) cpu->zf = 1; else cpu->zf = 0;
    if (cpu->res8 & 0x80) cpu->sf = 1; else cpu->sf = 0;
    cpu->pf = calc_parity(cpu->res8);
    if (dst & 0xFF00) cpu->cf = 1; else cpu->cf = 0;
    if (((v1 ^ imm) & (v1 ^ dst) & 0x80) == 0x80) cpu->of = 1; else cpu->of = 0;
    if (((v1 ^ imm ^ dst) & 0x10) == 0x10) cpu->af = 1; else cpu->af = 0;

    cpu->regs.byteregs[regal] = cpu->res8;
    return 0;
}

/* 0x1D: SBB AX, Iv — Subtract immediate16 + CF (borrow) from AX */
int dinstr_sbb_ax_imm16(CPU_t* cpu, uint16_t imm) {
    uint16_t v1 = cpu->regs.wordregs[regax];
    uint32_t dst = (uint32_t)v1 - (uint32_t)(imm + cpu->cf);

    cpu->res16 = (uint16_t)dst;
    if (!cpu->res16) cpu->zf = 1; else cpu->zf = 0;
    if (cpu->res16 & 0x8000) cpu->sf = 1; else cpu->sf = 0;
    cpu->pf = calc_parity(cpu->res16 & 0xFF);
    if (dst & 0xFFFF0000) cpu->cf = 1; else cpu->cf = 0;
    if (((v1 ^ imm) & (v1 ^ dst) & 0x8000) == 0x8000) cpu->of = 1; else cpu->of = 0;
    if (((v1 ^ imm ^ dst) & 0x10) == 0x10) cpu->af = 1; else cpu->af = 0;

    cpu->regs.wordregs[regax] = cpu->res16;
    return 0;
}

/* 0x24: AND AL, Ib — AND AL with immediate8, store in AL */
int dinstr_and_al_imm8(CPU_t* cpu, uint8_t imm) {
    uint8_t res = cpu->regs.byteregs[regal] & imm;
    if (!res) cpu->zf = 1; else cpu->zf = 0;
    if (res & 0x80) cpu->sf = 1; else cpu->sf = 0;
    cpu->pf = calc_parity(res);
    cpu->cf = 0;
    cpu->of = 0;
    cpu->regs.byteregs[regal] = res;
    return 0;
}

/* 0x25: AND AX, Iv — AND AX with immediate16, store in AX */
int dinstr_and_ax_imm16(CPU_t* cpu, uint16_t imm) {
    uint16_t res = cpu->regs.wordregs[regax] & imm;
    if (!res) cpu->zf = 1; else cpu->zf = 0;
    if (res & 0x8000) cpu->sf = 1; else cpu->sf = 0;
    cpu->pf = calc_parity(res & 0xFF);
    cpu->cf = 0;
    cpu->of = 0;
    cpu->regs.wordregs[regax] = res;
    return 0;
}
