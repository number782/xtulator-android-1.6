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
#include <android/log.h>
#include "cpu.h"
#include "../memory.h"
#include "../chipset/i8259.h"
#include "../memory.h"
#include "dynrec.h"

extern const uint8_t byteregtable[8];
static uint8_t calc_parity(uint8_t val);

/* ---- Simple opcodes ---- */

int dinstr_nop(CPU_t* cpu) {
    (void)cpu;
    return 0;  /* NOP does nothing, always continue */
}

int dinstr_hlt(CPU_t* cpu) {
    cpu->hltstate = 1;
    return 0;
}

/* Advance cpu->ip by n bytes.
 *
 * The interpreter in cpu_exec() steps the IP inside its dispatch loop
 * (StepIP), so every native helper below can ignore it. The dynarec has
 * no such loop: the generated ARM block just calls helpers back to back,
 * so the IP has to be advanced explicitly. Without this, cpu->ip is still
 * pointing at the FIRST instruction of the block when a later helper runs
 * — which makes dinstr_exec_one() re-execute that first instruction and
 * every helper that reads ip (call/ret/jmp/int) compute from the wrong
 * address.
 *
 * The translator emits one call to this per translated instruction, and
 * always *before* any helper that consumes ip (exec_one, jmp, call, ret,
 * int), so those helpers observe a correct IP.
 */
int dinstr_advance_ip(CPU_t* cpu, uint16_t n) {
    cpu->ip = (uint16_t)(cpu->ip + n);
    return 0;
}

/* Execute one instruction via the interpreter — used for opcodes not
 * natively translated by the dynrec (ModR/M instructions, etc.)
 * Returns the number of bytes the instruction consumed (1-3). */
#ifdef HOSTTEST
uint64_t dr_hist[256], dr_hist_rep[256], dr_hist_0f[256], dr_hist_calls;
static void dr_hist_note(CPU_t* cpu) {
    uint32_t a = ((uint32_t)cpu->segregs[regcs] << 4) + cpu->ip; uint8_t b; int rep = 0, n = 0;
    for (;;) { b = cpu_read(cpu, a++); if (b==0x26||b==0x2E||b==0x36||b==0x3E||b==0x66||b==0x67||b==0x64||b==0x65) { if (++n>6) break; continue; }
               if (b==0xF2||b==0xF3) { rep=1; if (++n>6) break; continue; } break; }
    dr_hist_calls++;
    if (b==0x0F) dr_hist_0f[cpu_read(cpu,a)]++; else if (rep) dr_hist_rep[b]++; else dr_hist[b]++;
}
#endif
int dinstr_exec_one(CPU_t* cpu) {
    uint16_t ip_before = cpu->ip;
#ifdef HOSTTEST
    dr_hist_note(cpu);
#endif
    cpu_exec(cpu, 1);
    return cpu->ip - ip_before;
}

/* Store one byte (or two, for a word) without per-byte block invalidation.
 * Callers invalidate the span they touched once. Falls back to cpu_write for
 * addresses with no direct RAM mapping (MMIO), which invalidates itself. */
static void rep_store(CPU_t* cpu, uint32_t addr, uint8_t lo, uint8_t hi, int word,
                      uint32_t* wlo, uint32_t* whi) {
    uint32_t a = addr & MEMORY_MASK;
    uint8_t* p = memory_mapWrite[a];
    if (p != NULL) { *p = lo; }
    else { cpu_write(cpu, addr, lo); }
    if (a < *wlo) *wlo = a;
    if (a > *whi) *whi = a;
    if (word) {
        uint32_t b = (addr + 1) & MEMORY_MASK;
        uint8_t* q = memory_mapWrite[b];
        if (q != NULL) { *q = hi; }
        else { cpu_write(cpu, addr + 1, hi); }
        if (b < *wlo) *wlo = b;
        if (b > *whi) *whi = b;
    }
}

/* One interpreter instruction, reporting whether it invalidated any block.
 * Returns nonzero if the translated block that called us is now stale. */
extern volatile uint32_t dr_inval_epoch;
int dinstr_exec_fb(CPU_t* cpu) {
    uint32_t before = dr_inval_epoch;
    cpu_exec(cpu, 1);
    return dr_inval_epoch != before;
}

/* REP-prefixed MOVS/STOS/SCAS (no segment override), run to completion in C.
 * The interpreter executes ONE iteration per dispatch (it rewinds IP to the
 * prefix), so a REP STOSW of 4000 words cost 4000 trips through cpu_exec.
 * reptype: 1 = REP/REPE (F3), 2 = REPNE (F2). len = prefix + opcode bytes.
 * Flags for SCAS come from cpu_alu8/16 with op 7 (CMP), the same routine the
 * interpreter's flag_sub uses. */
int dinstr_rep_string(CPU_t* cpu, uint32_t opcode, uint32_t reptype, uint32_t len) {
    /* Stores go straight to the RAM byte, and the span written is tracked so
     * the block cache is invalidated ONCE at the end. Per-byte invalidation
     * scanned every block for every byte of a 4000-word STOSW (measured: the
     * same boot went from 41 s to 62 s with per-byte invalidation). */
    uint32_t wlo = 0xFFFFFFFFu, whi = 0;
    uint32_t dsb = segbase(cpu->segregs[regds]);
    uint32_t esb = segbase(cpu->segregs[reges]);
    int word = (opcode & 1) != 0;                 /* A5/AB/AF are word forms */
    int elem = word ? 2 : 1;
    int step = cpu->df ? -elem : elem;
    int is_scas = (opcode == 0xAE || opcode == 0xAF);

    while (cpu->regs.wordregs[regcx] != 0) {
        uint16_t di = cpu->regs.wordregs[regdi];
        uint16_t si = cpu->regs.wordregs[regsi];
        switch (opcode) {
        case 0xA4: case 0xA5:             /* MOVSB / MOVSW */
            if (word) { uint16_t v = cpu_readw(cpu, dsb + si); rep_store(cpu, esb + di, (uint8_t)v, (uint8_t)(v >> 8), 1, &wlo, &whi); }
            else      { rep_store(cpu, esb + di, cpu_read(cpu, dsb + si), 0, 0, &wlo, &whi); }
            cpu->regs.wordregs[regsi] = (uint16_t)(si + step);
            break;
        case 0xAA: case 0xAB:             /* STOSB / STOSW */
            if (word) rep_store(cpu, esb + di, cpu->regs.wordregs[regax] & 0xFF, cpu->regs.wordregs[regax] >> 8, 1, &wlo, &whi);
            else      rep_store(cpu, esb + di, cpu->regs.byteregs[regal], 0, 0, &wlo, &whi);
            break;
        case 0xAE:                         /* SCASB */
            (void)cpu_alu8(cpu, 7, cpu->regs.byteregs[regal], cpu_read(cpu, esb + di));
            break;
        case 0xAF:                         /* SCASW */
            (void)cpu_alu16(cpu, 7, cpu->regs.wordregs[regax], cpu_readw(cpu, esb + di));
            break;
        default:
            return 0;
        }
        cpu->regs.wordregs[regdi] = (uint16_t)(di + step);
        cpu->regs.wordregs[regcx]--;
        if (is_scas) {
            if (reptype == 1 && !cpu->zf) break;   /* REPE stops on mismatch */
            if (reptype == 2 && cpu->zf) break;    /* REPNE stops on match */
        }
    }
    if (whi >= wlo) dynrec_invalidate_range(wlo, whi - wlo + 1);
    cpu->ip = (uint16_t)(cpu->ip + len);
    return 0;
}

/* ---- Register 16-bit operations ---- */

int dinstr_inc_r16(CPU_t* cpu, uint8_t regnum) {
    /* Mirrors interpreter INC: ADD with 1, every flag but CF updated. */
    uint16_t v1 = cpu->regs.wordregs[regnum & 7];
    uint32_t dst = (uint32_t)v1 + 1u;
    uint16_t res = (uint16_t)dst;
    cpu->zf = !res;
    cpu->sf = (res & 0x8000) ? 1 : 0;
    cpu->pf = calc_parity(res & 0xFF);
    cpu->of = (((dst ^ v1) & (dst ^ 1u) & 0x8000) == 0x8000);
    cpu->af = (((v1 ^ 1u ^ dst) & 0x10) == 0x10);
    cpu->regs.wordregs[regnum & 7] = res;
    return 0;
}

int dinstr_dec_r16(CPU_t* cpu, uint8_t regnum) {
    /* Mirrors interpreter DEC: SUB 1, every flag but CF updated. */
    uint16_t v1 = cpu->regs.wordregs[regnum & 7];
    uint32_t dst = (uint32_t)v1 - 1u;
    uint16_t res = (uint16_t)dst;
    cpu->zf = !res;
    cpu->sf = (res & 0x8000) ? 1 : 0;
    cpu->pf = calc_parity(res & 0xFF);
    cpu->of = (((dst ^ v1) & (v1 ^ 1u) & 0x8000) == 0x8000);
    cpu->af = (((v1 ^ 1u ^ dst) & 0x10) == 0x10);
    cpu->regs.wordregs[regnum & 7] = res;
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

/* 0xB0-0xB7: MOV r8, Ib — opcode encodes register as: 0=AL,1=CL,2=DL,3=BL,4=AH,5=CH,6=DH,7=BH */
int dinstr_mov_r8_imm8(CPU_t* cpu, uint8_t regnum, uint8_t imm) {
    cpu->regs.byteregs[byteregtable[regnum]] = imm;
    return 0;
}

/* ---- Control flow ---- */

int dinstr_jmp_rel8(CPU_t* cpu, uint32_t rel_raw) {
    int8_t rel = (int8_t)rel_raw;  /* callers pass a zero-extended byte; sign-extend here */
    cpu->ip += 2;
    cpu->ip += rel;
    /* JMP terminates the basic block */
    return 1;
}

int dinstr_jcond_rel8(CPU_t* cpu, uint32_t opcode, uint32_t rel_raw) {
    int8_t rel = (int8_t)rel_raw;  /* callers pass a zero-extended byte; sign-extend here */
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

int dinstr_jmp_rel16(CPU_t* cpu, uint32_t rel_raw) {
    int16_t rel = (int16_t)rel_raw;
    cpu->ip += 3;
    cpu->ip += rel;
    /* JMP terminates the basic block */
    return 1;
}

int dinstr_call_rel16(CPU_t* cpu, uint32_t rel_raw) {
    int16_t rel = (int16_t)rel_raw;
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

/* ---- LOOP/LOOPE/LOOPNE/JCXZ (0xE0-0xE3) ---- */

/* 0xE0: LOOPNE/JCXZ-with-conditions — loop if CX!=0 and ZF==0 */
/* 0xE1: LOOPE/LOOPZ — loop if CX!=0 and ZF==1 */
/* 0xE2: LOOP — loop if CX!=0 */
/* 0xE3: JCXZ — jump if CX==0 */
/* opcode: 0xE0 (LOOPNZ), 0xE1 (LOOPZ), 0xE2 (LOOP), 0xE3 (JCXZ)
 * rel: signed 8-bit displacement */
int dinstr_loop_rel8(CPU_t* cpu, uint32_t opcode, uint32_t rel_raw) {
    int8_t rel = (int8_t)rel_raw;
    cpu->ip += 2;  /* advance past opcode + rel8 */
    cpu->regs.wordregs[regcx] = (uint16_t)(cpu->regs.wordregs[regcx] - 1);
    switch (opcode) {
    case 0xE0:  /* LOOPNE: CX!=0 && ZF==0 */
        if (cpu->regs.wordregs[regcx] && !cpu->zf) cpu->ip += rel;
        break;
    case 0xE1:  /* LOOPZ: CX!=0 && ZF==1 */
        if (cpu->regs.wordregs[regcx] && cpu->zf) cpu->ip += rel;
        break;
    case 0xE2:  /* LOOP: CX!=0 */
        if (cpu->regs.wordregs[regcx]) cpu->ip += rel;
        break;
    case 0xE3:  /* JCXZ: CX==0 (CX NOT decremented for JCXZ) */
        /* JCXZ doesn't decrement CX; undo the decrement above */
        cpu->regs.wordregs[regcx] = (uint16_t)(cpu->regs.wordregs[regcx] + 1);
        if (cpu->regs.wordregs[regcx] == 0) cpu->ip += rel;
        break;
    }
    return 1;  /* loop/control-flow terminates the basic block */
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
    /* Same image as the interpreter's 0x9C: the V20/8086 build sets the high
     * flag bits (0xF800 / 0x0800), which software such as CPU detection reads. */
#ifdef CPU_SET_HIGH_FLAGS
    putmem16(cpu, cpu->segregs[regss], cpu->regs.wordregs[regsp], makeflagsword(cpu) | 0xF800);
#else
    putmem16(cpu, cpu->segregs[regss], cpu->regs.wordregs[regsp], makeflagsword(cpu) | 0x0800);
#endif
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

int dinstr_add_r16_imm(CPU_t* cpu, uint32_t regnum, uint32_t imm) {
    cpu->regs.wordregs[regnum & 7] = cpu_alu16(cpu, 0, cpu->regs.wordregs[regnum & 7], (uint16_t)imm);
    return 0;
}

/* 0x04: ADD AL, Ib — Add immediate8 to AL, set all flags */
int dinstr_add_al_imm8(CPU_t* cpu, uint32_t imm) {
    cpu->regs.byteregs[regal] = cpu_alu8(cpu, 0, cpu->regs.byteregs[regal], (uint8_t)imm);
    return 0;
}

/* 0x05: ADD AX, Iv — Add immediate16 to AX, set all flags */
int dinstr_add_ax_imm16(CPU_t* cpu, uint32_t imm) {
    cpu->regs.wordregs[regax] = cpu_alu16(cpu, 0, cpu->regs.wordregs[regax], (uint16_t)imm);
    return 0;
}

int dinstr_cmp_r16_imm(CPU_t* cpu, uint32_t regnum, uint32_t imm) {
    (void)cpu_alu16(cpu, 7, cpu->regs.wordregs[regnum & 7], (uint16_t)imm);
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
int dinstr_or_al_imm8(CPU_t* cpu, uint32_t imm) {
    cpu->regs.byteregs[regal] = cpu_alu8(cpu, 1, cpu->regs.byteregs[regal], (uint8_t)imm);
    return 0;
}

/* 0x0D: OR AX, Iv — OR AX with immediate16, store in AX */
int dinstr_or_ax_imm16(CPU_t* cpu, uint32_t imm) {
    cpu->regs.wordregs[regax] = cpu_alu16(cpu, 1, cpu->regs.wordregs[regax], (uint16_t)imm);
    return 0;
}

/* 0x34: XOR AL, Ib — XOR AL with immediate8, store in AL */
int dinstr_xor_al_imm8(CPU_t* cpu, uint32_t imm) {
    cpu->regs.byteregs[regal] = cpu_alu8(cpu, 6, cpu->regs.byteregs[regal], (uint8_t)imm);
    return 0;
}

/* 0x35: XOR AX, Iv — XOR AX with immediate16, store in AX */
int dinstr_xor_ax_imm16(CPU_t* cpu, uint32_t imm) {
    cpu->regs.wordregs[regax] = cpu_alu16(cpu, 6, cpu->regs.wordregs[regax], (uint16_t)imm);
    return 0;
}

/* ---- SUB immediate ---- */

/* 0x2C: SUB AL, Ib — Subtract immediate8 from AL, set all flags */
int dinstr_sub_al_imm8(CPU_t* cpu, uint32_t imm) {
    cpu->regs.byteregs[regal] = cpu_alu8(cpu, 5, cpu->regs.byteregs[regal], (uint8_t)imm);
    return 0;
}

/* 0x2D: SUB AX, Iv — Subtract immediate16 from AX, set all flags */
int dinstr_sub_ax_imm16(CPU_t* cpu, uint32_t imm) {
    cpu->regs.wordregs[regax] = cpu_alu16(cpu, 5, cpu->regs.wordregs[regax], (uint16_t)imm);
    return 0;
}

/* ---- CMP immediate ---- */

/* 0x3C: CMP AL, Ib — Subtract immediate8 from AL, set flags, discard result */
int dinstr_cmp_al_imm8(CPU_t* cpu, uint32_t imm) {
    (void)cpu_alu8(cpu, 7, cpu->regs.byteregs[regal], (uint8_t)imm);
    return 0;
}

/* 0x3D: CMP AX, Iv — Subtract immediate16 from AX, set flags, discard result */
int dinstr_cmp_ax_imm16(CPU_t* cpu, uint32_t imm) {
    (void)cpu_alu16(cpu, 7, cpu->regs.wordregs[regax], (uint16_t)imm);
    return 0;
}

/* 0x14: ADC AL, Ib — Add AL + immediate8 + CF, store in AL */
int dinstr_adc_al_imm8(CPU_t* cpu, uint32_t imm) {
    cpu->regs.byteregs[regal] = cpu_alu8(cpu, 2, cpu->regs.byteregs[regal], (uint8_t)imm);
    return 0;
}

/* 0x15: ADC AX, Iv — Add AX + immediate16 + CF, store in AX */
int dinstr_adc_ax_imm16(CPU_t* cpu, uint32_t imm) {
    cpu->regs.wordregs[regax] = cpu_alu16(cpu, 2, cpu->regs.wordregs[regax], (uint16_t)imm);
    return 0;
}

/* 0x1C: SBB AL, Ib — Subtract immediate8 + CF (borrow) from AL */
int dinstr_sbb_al_imm8(CPU_t* cpu, uint32_t imm) {
    cpu->regs.byteregs[regal] = cpu_alu8(cpu, 3, cpu->regs.byteregs[regal], (uint8_t)imm);
    return 0;
}

/* 0x1D: SBB AX, Iv — Subtract immediate16 + CF (borrow) from AX */
int dinstr_sbb_ax_imm16(CPU_t* cpu, uint32_t imm) {
    cpu->regs.wordregs[regax] = cpu_alu16(cpu, 3, cpu->regs.wordregs[regax], (uint16_t)imm);
    return 0;
}

/* 0x24: AND AL, Ib — AND AL with immediate8, store in AL */
int dinstr_and_al_imm8(CPU_t* cpu, uint32_t imm) {
    cpu->regs.byteregs[regal] = cpu_alu8(cpu, 4, cpu->regs.byteregs[regal], (uint8_t)imm);
    return 0;
}

/* 0x25: AND AX, Iv — AND AX with immediate16, store in AX */
int dinstr_and_ax_imm16(CPU_t* cpu, uint32_t imm) {
    cpu->regs.wordregs[regax] = cpu_alu16(cpu, 4, cpu->regs.wordregs[regax], (uint16_t)imm);
    return 0;
}

/* 0xA8: TEST AL, Ib — AND AL with immediate8, set flags, discard result */
int dinstr_test_al_imm8(CPU_t* cpu, uint32_t imm) {
    cpu_test8(cpu, cpu->regs.byteregs[regal], (uint8_t)imm);
    return 0;
}

/* 0xA9: TEST AX, Iv — AND AX with immediate16, set flags, discard result */
int dinstr_test_ax_imm16(CPU_t* cpu, uint32_t imm) {
    cpu_test16(cpu, cpu->regs.wordregs[regax], (uint16_t)imm);
    return 0;
}
