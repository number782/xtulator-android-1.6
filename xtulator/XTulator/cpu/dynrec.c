/*
    ARMv5TE Dynamic Recompiler for XTulator 8088 core.

    Architecture:
    - Code cache: mmap RWX memory, 512KB
    - Block cache: hash table (PC → translated block)
    - Translation: x86 instructions → ARM BL calls to C helpers
    - Execution: cast code cache pointer to function pointer, call it

    The generated ARM block:
        stmfd  sp!, {r4, r10, r11, lr}   @ save callee-saved
        mov    r10, #N                   @ instruction counter
    loop:
        bl     helper_op_XX             @ execute x86 instruction
        subs   r10, r10, #1              @ decrement counter
        bne    loop                     @ more instructions
        ldmfd  sp!, {r4, r10, r11, pc}   @ restore and return

    The C helpers use R4=cpu as first arg (per AAPCS, R4 is callee-saved
    so it survives across helper calls).
*/

#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "../config.h"
#include "cpu.h"
#include "dynrec.h"
#include "../chipset/i8259.h"
#include "../memory.h"
#include "../debuglog.h"

#ifdef __ANDROID__
#include <android/log.h>
#define DLOGI(...) do { if (trace_flags & TRACE_FLAG_DIAG) { __android_log_print(ANDROID_LOG_INFO, "XTulator-DYNREC", __VA_ARGS__); } } while (0)
#else
#define DLOGI(...)
#endif

#ifdef __ARM_ARCH_5TE__

/* Reduced from 512KB to 256KB to fit within IS01's memory constraints */
#define CODE_CACHE_SIZE    (256 * 1024)
#define MAX_BLOCKS         1024
#define MAX_INSTRUCTIONS   128

int dynrec_enabled = 0;

/* ---- Code cache ---- */
static uint8_t* code_cache_base = NULL;
static uint32_t code_cache_pos = 0;

/* ---- Block cache ---- */
typedef struct {
    uint32_t x86_pc;      /* CS*16 + IP */
    uint32_t arm_offset;  /* offset in code cache */
    uint32_t code_len;
    uint32_t x86_len;
    uint32_t x86_end_ip;  /* expected IP after block (if no control flow) */
} block_entry_t;

static block_entry_t block_cache[MAX_BLOCKS];
static int block_cache_count = 0;

/* ---- Dynrec statistics (for diagnostics) ---- */
volatile uint32_t dynrec_native_blocks = 0;    /* count of ARM blocks executed natively */
volatile uint64_t dynrec_interpreter_instrs = 0;  /* count of interpreter-fallback instructions */

/* ---- ARM instruction emission ---- */

#define ARM_COND_AL  0xE0000000  /* condition: always */

static int emit32_failed = 0;

static void emit32(uint32_t instr) {
    if (emit32_failed) return;
    if (code_cache_pos + 4 > CODE_CACHE_SIZE) {
        emit32_failed = 1;
        DLOGI("emit32: code cache overflow at pos=%u", code_cache_pos);
        return;
    }
    *((uint32_t*)(code_cache_base + code_cache_pos)) = instr;
    code_cache_pos += 4;
}

/* MOV Rd, Rm (register to register) */
static void emit_mov_reg(int Rd, int Rm) {
    emit32(ARM_COND_AL | 0xE1A00000 | (Rd << 12) | Rm);
}

/* ARM register assignments for generated code:
   R0-R3:  scratch/args for helper calls
   R4:     CPU_t* (callee-saved, set in prologue, passed implicitly)
   R10:    instruction counter
   R11:    I8259_t* (callee-saved)
   R12:    scratch
   R13:    SP (must be 8-byte aligned for ARM)
   R14:    LR
   R15:    PC

   The generated block function signature is: void block(CPU_t* cpu)
   The cpu argument arrives in R0 (AAPCS). We save it into R4 in the
   prologue. Helper functions receive CPU_t* implicitly via R4 (NOT
   via R0 as a parameter), plus any extra args in R1/R2/...

   This means helper function signatures are: int dinstr_xx(CPU_t* cpu, ...)
   but the "cpu" parameter is actually R4, not R0. The calling code must
   NOT clobber R4 before the BL call.
*/

/*
   To pass the cpu pointer to helpers via R4, we need to load R4 in the
   prologue. But the helpers are C functions expecting cpu as first arg (R0).
   We have two options:
   1. Pass cpu in R0 and save extra args in other registers
   2. Use R4 as an implicit cpu pointer and have helpers use it

   Approach 2 is cleaner but requires helpers to be modified to not expect cpu in R0.
   However, since we can't easily declare a function that reads R4 directly in C,
   we use Approach 1: pass cpu in R0 (from the function call convention), and
    for helpers that need additional args, move cpu to R4 first, then set R0-R1.
*/

/* STMFD sp!, {r4, r10, r11, lr} */
#define ARM_PUSH() emit32(0xE92D4C10)  /* r4=bit4, r10=bit10, r11=bit11, lr=bit14 */
/* LDMFD sp!, {r4, r10, r11, pc} */
#define ARM_POP() emit32(0xE8BD8C10)

/* MOV Rd, #imm8 */
static void emit_mov_imm(int Rd, uint8_t imm) {
    emit32(ARM_COND_AL | 0x3A00000 | (Rd << 12) | imm);
}

/* MOVW Rd, #imm16 — load 16-bit immediate (ARMv5TE doesn't have MOVW, so use MOV+ORR) */
static void emit_movw(int Rd, uint16_t imm) {
    uint32_t lo = imm & 0xFF;
    uint32_t hi = (imm >> 8) & 0xFF;
    if (hi == 0) {
        emit32(ARM_COND_AL | 0x3A00000 | (Rd << 12) | lo);  /* MOV Rd, #lo */
    } else if (lo == 0) {
        /* Encode hi << 8 using rotate: ROR 24 (rotate_imm=12) of imm8 */
        emit32(ARM_COND_AL | 0x3A00000 | (Rd << 12) | (12 << 7) | hi);  /* MOV Rd, #hi, LSL #8 */
    } else {
        /* MOV Rd, #lo; ORR Rd, Rd, #hi << 8 */
        emit32(ARM_COND_AL | 0x3A00000 | (Rd << 12) | lo);  /* MOV Rd, #lo */
        emit32(ARM_COND_AL | 0x3800000 | (Rd << 12) | (Rd << 16) | (12 << 7) | hi);  /* ORR Rd, Rd, #hi, LSL #8 */
    }
}

/* LDR r0, =imm32 (literal pool) - not currently used */
static void emit_ldr_imm32(int Rd, uint32_t imm) {
    /* MOV + ORR approach for arbitrary 32-bit immediate */
    uint8_t b0 = imm & 0xFF;
    uint8_t b1 = (imm >> 8) & 0xFF;
    uint8_t b2 = (imm >> 16) & 0xFF;
    uint8_t b3 = (imm >> 24) & 0xFF;
    if (b1 == 0 && b2 == 0 && b3 == 0) {
        emit32(ARM_COND_AL | 0x3A00000 | (Rd << 12) | b0);  /* MOV Rd, #b0 */
    } else if (b2 == 0 && b3 == 0) {
        emit32(ARM_COND_AL | 0x3A00000 | (Rd << 12) | b0);   /* MOV Rd, #b0 */
        if (b1 != 0) {
            emit32(ARM_COND_AL | 0x3800000 | (Rd << 12) | (Rd << 16) | (12 << 7) | b1);  /* ORR Rd, Rd, #b1, LSL #8 */
        }
    }
    /* For 3-4 byte immediates, would need additional ORR instructions with rotates */
}

/* BL to target — computes offset automatically at emit time */
static void emit_bl(uint32_t target_addr) {
    uint32_t pc = code_cache_pos;
    /* On ARM, PC reads as instruction_address + 8 during execution */
    uint32_t base_pc_plus_8 = (uint32_t)code_cache_base + pc + 8;
    int32_t offset = (int32_t)(target_addr - base_pc_plus_8) / 4;

    if (offset > 8388607 || offset < -8388608) {
        /* Target is out of BL's ±32MB range (24-bit signed offset).
         * Use indirect call via literal pool:
         *   A:   LDR R12, [PC, #4]   → PC=A+8, loads from A+12 (target_addr)
         *   A+4: BLX R12             → LR=A+8, calls target
         *   A+8: B 0                 → NOP (skip literal pool)
         *   A+12: .word target_addr  → literal pool (never executed)
         * After BLX returns to A+8, B 0 is a NOP → continues at caller's next instr. */
        emit32(0xE59FC004);     /* LDR R12, [PC, #4] */
        emit32(0xE12FFF3C);     /* BLX R12 */
        emit32(0xEA000000);     /* B 0 (NOP) — skips over literal pool */
        emit32(target_addr);    /* literal pool entry */
        DLOGI("emit_bl: indirect call for target=0x%08X", target_addr);
    } else {
        emit32(0xEB000000 | (offset & 0xFFFFFF));
    }
}

/* Placeholder macro for compatibility */
#define ARM_BL_PLACEHOLDER() (NULL)

/* B label (unconditional branch) */
static void emit_b_uncond(uint32_t* patch_slot) {
    emit32(ARM_COND_AL | 0x0A000000);  /* B placeholder */
    *patch_slot = code_cache_pos - 4;
}

/* SUBS r10, r10, #imm8 */
static void emit_subs_imm(uint8_t imm) {
    emit32(0xE250A000 | imm);  /* SUBS r10, r10, #imm8 */
}

/* BNE target — patch later */
static uint32_t emit_bne_placeholder(void) {
    emit32(0x1A000000);  /* BNE placeholder (cond=0001=NE) */
    return code_cache_pos - 4;
}

/* Patch a branch at pos to target */
static void patch_branch(uint32_t pos, uint32_t target) {
    /* On ARM, PC reads as instruction_address + 8 during execution */
    int32_t offset = (int32_t)(target - ((uint32_t)code_cache_base + pos + 8)) / 4;
    offset &= 0xFFFFFF;
    uint32_t orig = *((uint32_t*)(code_cache_base + pos));
    orig = (orig & 0xFF000000) | offset;
    *((uint32_t*)(code_cache_base + pos)) = orig;
}

/* Patch a BL at pos to target */
static void patch_bl(uint32_t pos, uint32_t target) {
    /* On ARM, PC reads as instruction_address + 8 during execution */
    int32_t offset = (int32_t)(target - ((uint32_t)code_cache_base + pos + 8)) / 4;
    offset &= 0xFFFFFF;
    uint32_t orig = *((uint32_t*)(code_cache_base + pos));
    orig = (orig & 0xFF000000) | offset;
    *((uint32_t*)(code_cache_base + pos)) = orig;
}

/* ---- Block cache ---- */

static block_entry_t* find_block(uint32_t x86_pc) {
    for (int i = 0; i < block_cache_count; i++) {
        if (block_cache[i].x86_pc == x86_pc) {
            return &block_cache[i];
        }
    }
    return NULL;
}

static void invalidate_block(uint32_t x86_pc) {
    for (int i = 0; i < block_cache_count; i++) {
        if (block_cache[i].x86_pc == x86_pc) {
            block_cache[i].x86_pc = 0;
        }
    }
}

/* ---- Helper function declarations ---- */

/* Each helper takes (CPU_t* cpu) in R0, returns 0=ok, 1=break out of block */
extern int dinstr_nop(CPU_t* cpu);
extern int dinstr_mov_r_i(CPU_t* cpu, uint8_t regnum, uint16_t imm);
extern int dinstr_inc_r16(CPU_t* cpu, uint8_t regnum);
extern int dinstr_dec_r16(CPU_t* cpu, uint8_t regnum);
extern int dinstr_push_r16(CPU_t* cpu, uint8_t regnum);
extern int dinstr_pop_r16(CPU_t* cpu, uint8_t regnum);
extern int dinstr_hlt(CPU_t* cpu);
extern int dinstr_int(CPU_t* cpu, uint8_t intnum);
extern int dinstr_jmp_rel8(CPU_t* cpu, int8_t rel);
extern int dinstr_jcond_rel8(CPU_t* cpu, uint8_t opcode, int8_t rel);
extern int dinstr_jmp_rel16(CPU_t* cpu, int16_t rel);
extern int dinstr_call_rel16(CPU_t* cpu, int16_t rel);
extern int dinstr_ret_near(CPU_t* cpu);
extern int dinstr_add_r16_imm(CPU_t* cpu, uint8_t regnum, uint16_t imm);
extern int dinstr_cmp_r16_imm(CPU_t* cpu, uint8_t regnum, uint16_t imm);
extern int dinstr_add_al_imm8(CPU_t* cpu, uint8_t imm);
extern int dinstr_add_ax_imm16(CPU_t* cpu, uint16_t imm);
extern int dinstr_pushf(CPU_t* cpu);
extern int dinstr_popf(CPU_t* cpu);
extern int dinstr_inc_r8(CPU_t* cpu, uint8_t regnum);
extern int dinstr_dec_r8(CPU_t* cpu, uint8_t regnum);
extern int dinstr_add_r16_imm(CPU_t* cpu, uint8_t regnum, uint16_t imm);
extern int dinstr_cmp_r16_imm(CPU_t* cpu, uint8_t regnum, uint16_t imm);
extern int dinstr_push_es(CPU_t* cpu);
extern int dinstr_pop_es(CPU_t* cpu);
extern int dinstr_push_cs(CPU_t* cpu);
extern int dinstr_pop_cs(CPU_t* cpu);
extern int dinstr_push_ss(CPU_t* cpu);
extern int dinstr_pop_ss(CPU_t* cpu);
extern int dinstr_push_ds(CPU_t* cpu);
extern int dinstr_pop_ds(CPU_t* cpu);
extern int dinstr_sahf(CPU_t* cpu);
extern int dinstr_lahf(CPU_t* cpu);

/* Opcodes 0x80-0x8F: ModR/M-based group and data-movement opcodes */
extern int dinstr_test_gb_eb(CPU_t* cpu);
extern int dinstr_test_gv_ev(CPU_t* cpu);
extern int dinstr_mov_ew_sw(CPU_t* cpu);
extern int dinstr_lea_gv_m(CPU_t* cpu);
extern int dinstr_mov_sw_ew(CPU_t* cpu);
extern int dinstr_cmc(CPU_t* cpu);
extern int dinstr_clc(CPU_t* cpu);
extern int dinstr_stc(CPU_t* cpu);
extern int dinstr_cli(CPU_t* cpu);
extern int dinstr_sti(CPU_t* cpu);
extern int dinstr_cld(CPU_t* cpu);
extern int dinstr_std(CPU_t* cpu);
extern int dinstr_or_al_imm8(CPU_t* cpu, uint8_t imm);
extern int dinstr_or_ax_imm16(CPU_t* cpu, uint16_t imm);
extern int dinstr_xor_al_imm8(CPU_t* cpu, uint8_t imm);
extern int dinstr_xor_ax_imm16(CPU_t* cpu, uint16_t imm);
extern int dinstr_sub_al_imm8(CPU_t* cpu, uint8_t imm);
extern int dinstr_sub_ax_imm16(CPU_t* cpu, uint16_t imm);
extern int dinstr_cmp_al_imm8(CPU_t* cpu, uint8_t imm);
extern int dinstr_cmp_ax_imm16(CPU_t* cpu, uint16_t imm);
extern int dinstr_adc_al_imm8(CPU_t* cpu, uint8_t imm);
extern int dinstr_adc_ax_imm16(CPU_t* cpu, uint16_t imm);
extern int dinstr_sbb_al_imm8(CPU_t* cpu, uint8_t imm);
extern int dinstr_sbb_ax_imm16(CPU_t* cpu, uint16_t imm);
extern int dinstr_and_al_imm8(CPU_t* cpu, uint8_t imm);
extern int dinstr_and_ax_imm16(CPU_t* cpu, uint16_t imm);
extern int dinstr_exec_one(CPU_t* cpu);

/* ---- Block translation ---- */

static int translate_block(CPU_t* cpu, uint32_t x86_pc, int max_instr) {
    uint8_t* mem = memory_mapRead[x86_pc & MEMORY_MASK];
    if (mem == NULL || memory_mapReadCallback[x86_pc & MEMORY_MASK] != NULL) {
        return -1;  /* Can't translate MMIO */
    }

    uint32_t start_pos = code_cache_pos;

    /* Check if block cache is full */
    if (block_cache_count >= MAX_BLOCKS) {
        DLOGI("translate_block: block cache full at x86_pc=0x%06X", x86_pc);
        return -1;
    }

    /* Reset emit overflow flag for this translation */
    emit32_failed = 0;

    /* Function prologue: save callee-saved registers and save cpu pointer */
    ARM_PUSH();  /* save r4, r10, r11, lr */
    emit_mov_reg(4, 0);  /* R4 = cpu (first argument) — preserve across all helper calls */

    int instr_count = 0;
    int offset = 0;
    int has_native_instrs = 0;  /* Track if any natively-translated instructions were emitted */

    while (instr_count < max_instr) {
        uint8_t opcode = mem[offset];

        /* Assume native instruction until proven otherwise */
        has_native_instrs = 1;

        switch (opcode) {
        /* ---- x86 prefix bytes — skip and continue to next opcode ---- */
        case 0x2E:  /* CS segment override prefix */
        case 0x3E:  /* DS segment override prefix */
        case 0x26:  /* ES segment override prefix */
        case 0x36:  /* SS segment override prefix */
        case 0x64:  /* FS segment override prefix */
        case 0x65:  /* GS segment override prefix */
        case 0x66:  /* Operand size override prefix */
        case 0x67:  /* Address size override prefix */
        case 0xF3:  /* REP/REPE/REPZ prefix */
        case 0xF2:  /* REPNE/REPNZ prefix */
            offset++;
            break;

        case 0x0F:  /* Two-byte opcode escape (0F xx) — defer to interpreter */
            emit_mov_reg(0, 4);
            emit_bl((uint32_t)&dinstr_exec_one);
            has_native_instrs = 0;
            offset++;
            instr_count++;
            goto block_done;
        /* ---- ADD opcodes (0x00-0x05) ---- */
        case 0x00: case 0x01: case 0x02: case 0x03:
            /* ADD Eb,Gb / ADD Ev,Gv / ADD Gb,Eb / ADD Gv,Ev — ModR/M forms */
            emit_mov_reg(0, 4);
            emit_bl((uint32_t)&dinstr_exec_one);
            has_native_instrs = 0;
            offset++;
            instr_count++;
            has_native_instrs = 0;
            goto block_done;

        case 0x04:  /* ADD AL, Ib */
            emit_mov_reg(0, 4);  /* R0 = cpu (saved in R4) */
            emit_mov_imm(1, mem[offset + 1]);  /* R1 = imm8 */
            emit_bl((uint32_t)&dinstr_add_al_imm8);
            offset += 2;
            break;

        case 0x05:  /* ADD AX, Iv */
            {
                uint16_t imm = mem[offset + 1] | (mem[offset + 2] << 8);
                emit_mov_reg(0, 4);  /* R0 = cpu */
                emit_movw(1, imm);  /* R1 = imm16 */
                emit_bl((uint32_t)&dinstr_add_ax_imm16);
            }
            offset += 3;
            break;

        /* ---- SUB opcodes (0x28-0x2D) ---- */
        case 0x28: case 0x29: case 0x2A: case 0x2B:
            /* SUB Eb,Gb / SUB Ev,Gv / SUB Gb,Eb / SUB Gv,Ev — ModR/M forms */
            emit_mov_reg(0, 4);
            emit_bl((uint32_t)&dinstr_exec_one);
            has_native_instrs = 0;
            offset++;
            instr_count++;
            goto block_done;

        case 0x2C:  /* SUB AL, Ib */
            emit_mov_reg(0, 4);
            emit_mov_imm(1, mem[offset + 1]);
            emit_bl((uint32_t)&dinstr_sub_al_imm8);
            offset += 2;
            break;

        case 0x2D:  /* SUB AX, Iv */
            {
                uint16_t imm = mem[offset + 1] | (mem[offset + 2] << 8);
                emit_mov_reg(0, 4);
                emit_movw(1, imm);
                emit_bl((uint32_t)&dinstr_sub_ax_imm16);
            }
            offset += 3;
            break;

        /* ---- CMP opcodes (0x38-0x3D) ---- */
        case 0x38: case 0x39: case 0x3A: case 0x3B:
            /* CMP Eb,Gb / CMP Ev,Gv / CMP Gb,Eb / CMP Gv,Ev — ModR/M forms */
            emit_mov_reg(0, 4);
            emit_bl((uint32_t)&dinstr_exec_one);
            has_native_instrs = 0;
            offset++;
            instr_count++;
            goto block_done;

        case 0x3C:  /* CMP AL, Ib */
            emit_mov_reg(0, 4);
            emit_mov_imm(1, mem[offset + 1]);
            emit_bl((uint32_t)&dinstr_cmp_al_imm8);
            offset += 2;
            break;

        case 0x3D:  /* CMP AX, Iv */
            {
                uint16_t imm = mem[offset + 1] | (mem[offset + 2] << 8);
                emit_mov_reg(0, 4);
                emit_movw(1, imm);
                emit_bl((uint32_t)&dinstr_cmp_ax_imm16);
            }
            offset += 3;
            break;

        case 0x90:  /* NOP */
            emit_mov_reg(0, 4);
            emit_bl((uint32_t)&dinstr_nop);
            offset++;
            break;

        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF: {
            /* MOV reg, imm16 */
            uint8_t regnum = opcode - 0xB8;
            emit_mov_reg(0, 4);  /* R0 = cpu */
            emit_movw(1, regnum);
            emit_movw(2, mem[offset + 1] | (mem[offset + 2] << 8));
            emit_bl((uint32_t)&dinstr_mov_r_i);
            offset += 3;
            break;
        }

        case 0x40: case 0x41: case 0x42: case 0x43:
        case 0x44: case 0x45: case 0x46: case 0x47:
            /* INC eXX */
            emit_mov_reg(0, 4);
            emit_movw(1, (uint16_t)(opcode - 0x40));
            emit_bl((uint32_t)&dinstr_inc_r16);
            offset++;
            break;

        case 0x48: case 0x49: case 0x4A: case 0x4B:
        case 0x4C: case 0x4D: case 0x4E: case 0x4F:
            /* DEC eXX */
            emit_mov_reg(0, 4);
            emit_movw(1, (uint16_t)(opcode - 0x48));
            emit_bl((uint32_t)&dinstr_dec_r16);
            offset++;
            break;

        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57:
            /* PUSH eXX */
            emit_mov_reg(0, 4);
            emit_movw(1, (uint16_t)(opcode - 0x50));
            emit_bl((uint32_t)&dinstr_push_r16);
            offset++;
            break;

        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            /* POP eXX */
            emit_mov_reg(0, 4);
            emit_movw(1, (uint16_t)(opcode - 0x58));
            emit_bl((uint32_t)&dinstr_pop_r16);
            offset++;
            break;

        case 0x9C:  /* PUSHF */
            emit_mov_reg(0, 4);
            emit_bl((uint32_t)&dinstr_pushf);
            offset++;
            break;

        case 0x9D:  /* POPF */
            emit_mov_reg(0, 4);
            emit_bl((uint32_t)&dinstr_popf);
            offset++;
            break;

        case 0xC3:  /* RET (near) */
            emit_mov_reg(0, 4);
            emit_bl((uint32_t)&dinstr_ret_near);
            offset++;
            instr_count++;
            goto block_done;

        case 0xCD:  /* INT imm8 */
            emit_mov_reg(0, 4);
            emit_mov_imm(1, mem[offset + 1]);
            emit_bl((uint32_t)&dinstr_int);
            offset += 2;
            instr_count++;
            goto block_done;

        case 0xE8:  /* CALL rel16 */
            {
                int16_t rel = mem[offset + 1] | (mem[offset + 2] << 8);
                emit_mov_reg(0, 4);
                emit_movw(1, (uint16_t)rel);
                emit_bl((uint32_t)&dinstr_call_rel16);
            }
            offset += 3;
            instr_count++;
            goto block_done;

        case 0xE9:  /* JMP rel16 */
            {
                int16_t rel = mem[offset + 1] | (mem[offset + 2] << 8);
                emit_mov_reg(0, 4);
                emit_movw(1, (uint16_t)rel);
                emit_bl((uint32_t)&dinstr_jmp_rel16);
            }
            offset += 3;
            instr_count++;
            goto block_done;

        case 0xEB:  /* JMP rel8 */
            emit_mov_reg(0, 4);
            emit_mov_imm(1, (uint8_t)mem[offset + 1]);
            emit_bl((uint32_t)&dinstr_jmp_rel8);
            offset += 2;
            instr_count++;
            goto block_done;

        case 0xF4:  /* HLT */
            emit_mov_reg(0, 4);
            emit_bl((uint32_t)&dinstr_hlt);
            offset++;
            instr_count++;
            goto block_done;

        /* ---- Segment register operations ---- */
        case 0x06:  /* PUSH ES */
            emit_mov_reg(0, 4);
            emit_bl((uint32_t)&dinstr_push_es);
            offset++;
            break;

        case 0x07:  /* POP ES */
            emit_mov_reg(0, 4);
            emit_bl((uint32_t)&dinstr_pop_es);
            offset++;
            break;

        /* ---- OR instructions ---- */
        case 0x08:  /* OR Eb,Gb */
        case 0x09:  /* OR Ev,Gv */
        case 0x0A:  /* OR Gb,Eb */
        case 0x0B:  /* OR Gv,Ev */
            emit_mov_reg(0, 4);
            emit_bl((uint32_t)&dinstr_exec_one);
            has_native_instrs = 0;
            offset++;
            instr_count++;
            goto block_done;

        case 0x0C:  /* OR AL,Ib */
            emit_mov_reg(0, 4);
            emit_mov_imm(1, mem[offset + 1]);
            emit_bl((uint32_t)&dinstr_or_al_imm8);
            offset += 2;
            break;

        case 0x0D:  /* OR AX,Iv */
            emit_mov_reg(0, 4);
            emit_movw(1, mem[offset + 1] | (mem[offset + 2] << 8));
            emit_bl((uint32_t)&dinstr_or_ax_imm16);
            offset += 3;
            break;

        case 0x0E:  /* PUSH CS */
            emit_mov_reg(0, 4);
            emit_bl((uint32_t)&dinstr_push_cs);
            offset++;
            break;

        /* ---- ADC instructions (0x10-0x15) ---- */
        case 0x10:  /* ADC Eb,Gb */
        case 0x11:  /* ADC Ev,Gv */
        case 0x12:  /* ADC Gb,Eb */
        case 0x13:  /* ADC Gv,Ev */
            emit_mov_reg(0, 4);
            emit_bl((uint32_t)&dinstr_exec_one);
            has_native_instrs = 0;
            offset++;
            instr_count++;
            goto block_done;

        case 0x14:  /* ADC AL,Ib */
            emit_mov_reg(0, 4);
            emit_mov_imm(1, mem[offset + 1]);
            emit_bl((uint32_t)&dinstr_adc_al_imm8);
            offset += 2;
            break;

        case 0x15:  /* ADC AX,Iv */
            emit_mov_reg(0, 4);
            emit_movw(1, mem[offset + 1] | (mem[offset + 2] << 8));
            emit_bl((uint32_t)&dinstr_adc_ax_imm16);
            offset += 3;
            break;

        /* ---- SBB instructions (0x18-0x1D) ---- */
        case 0x18:  /* SBB Eb,Gb */
        case 0x19:  /* SBB Ev,Gv */
        case 0x1A:  /* SBB Gb,Eb */
        case 0x1B:  /* SBB Gv,Ev */
            emit_mov_reg(0, 4);
            emit_bl((uint32_t)&dinstr_exec_one);
            has_native_instrs = 0;
            offset++;
            instr_count++;
            goto block_done;

        case 0x1C:  /* SBB AL,Ib */
            emit_mov_reg(0, 4);
            emit_mov_imm(1, mem[offset + 1]);
            emit_bl((uint32_t)&dinstr_sbb_al_imm8);
            offset += 2;
            break;

        case 0x1D:  /* SBB AX,Iv */
            emit_mov_reg(0, 4);
            emit_movw(1, mem[offset + 1] | (mem[offset + 2] << 8));
            emit_bl((uint32_t)&dinstr_sbb_ax_imm16);
            offset += 3;
            break;

        case 0x16:  /* PUSH SS */
            emit_mov_reg(0, 4);
            emit_bl((uint32_t)&dinstr_push_ss);
            offset++;
            break;

        case 0x17:  /* POP SS */
            emit_mov_reg(0, 4);
            emit_bl((uint32_t)&dinstr_pop_ss);
            offset++;
            break;

        case 0x1E:  /* PUSH DS */
            emit_mov_reg(0, 4);
            emit_bl((uint32_t)&dinstr_push_ds);
            offset++;
            break;

        case 0x1F:  /* POP DS */
            emit_mov_reg(0, 4);
            emit_bl((uint32_t)&dinstr_pop_ds);
            offset++;
            break;

        /* ---- XOR instructions ---- */
        case 0x30:  /* XOR Eb,Gb */
        case 0x31:  /* XOR Ev,Gv */
        case 0x32:  /* XOR Gb,Eb */
        case 0x33:  /* XOR Gv,Ev */
            emit_mov_reg(0, 4);
            emit_bl((uint32_t)&dinstr_exec_one);
            has_native_instrs = 0;
            offset++;
            instr_count++;
            goto block_done;

        case 0x34:  /* XOR AL,Ib */
            emit_mov_reg(0, 4);
            emit_mov_imm(1, mem[offset + 1]);
            emit_bl((uint32_t)&dinstr_xor_al_imm8);
            offset += 2;
            break;

        case 0x35:  /* XOR AX,Iv */
            emit_mov_reg(0, 4);
            emit_movw(1, mem[offset + 1] | (mem[offset + 2] << 8));
            emit_bl((uint32_t)&dinstr_xor_ax_imm16);
            offset += 3;
            break;

        /* ---- AND opcodes (0x20-0x25) ---- */
        case 0x20: case 0x21: case 0x22: case 0x23:
            /* AND Eb,Gb / AND Ev,Gv / AND Gb,Eb / AND Gv,Ev — ModR/M forms */
            emit_mov_reg(0, 4);
            emit_bl((uint32_t)&dinstr_exec_one);
            has_native_instrs = 0;
            offset++;
            instr_count++;
            goto block_done;

        case 0x24:  /* AND AL, Ib */
            emit_mov_reg(0, 4);
            emit_mov_imm(1, mem[offset + 1]);
            emit_bl((uint32_t)&dinstr_and_al_imm8);
            offset += 2;
            break;

        case 0x25:  /* AND AX, Iv */
            {
                uint16_t imm = mem[offset + 1] | (mem[offset + 2] << 8);
                emit_mov_reg(0, 4);
                emit_movw(1, imm);
                emit_bl((uint32_t)&dinstr_and_ax_imm16);
            }
            offset += 3;
            break;

        /* ---- SAHF/LAHF ---- */
        case 0x9E:  /* SAHF */
            emit_mov_reg(0, 4);
            emit_bl((uint32_t)&dinstr_sahf);
            offset++;
            break;

        case 0x9F:  /* LAHF */
            emit_mov_reg(0, 4);
            emit_bl((uint32_t)&dinstr_lahf);
            offset++;
            break;

        /* ---- Flag operations ---- */
        case 0xF0:  /* LOCK prefix — skip, no operation */
            offset++;
            break;

        case 0xF5:  /* CMC */
            emit_mov_reg(0, 4);
            emit_bl((uint32_t)&dinstr_cmc);
            offset++;
            break;

        case 0xF8:  /* CLC */
            emit_mov_reg(0, 4);
            emit_bl((uint32_t)&dinstr_clc);
            offset++;
            break;

        case 0xF9:  /* STC */
            emit_mov_reg(0, 4);
            emit_bl((uint32_t)&dinstr_stc);
            offset++;
            break;

        case 0xFA:  /* CLI */
            emit_mov_reg(0, 4);
            emit_bl((uint32_t)&dinstr_cli);
            offset++;
            break;

        case 0xFB:  /* STI */
            emit_mov_reg(0, 4);
            emit_bl((uint32_t)&dinstr_sti);
            offset++;
            break;

        case 0xFC:  /* CLD */
            emit_mov_reg(0, 4);
            emit_bl((uint32_t)&dinstr_cld);
            offset++;
            break;

        case 0xFD:  /* STD */
            emit_mov_reg(0, 4);
            emit_bl((uint32_t)&dinstr_std);
            offset++;
            break;

        /* ---- String operations — complex REP handling, fall back to interpreter ---- */
        case 0xA4:  /* MOVSB */
        case 0xA5:  /* MOVSW */
        case 0xAC:  /* LODSB */
        case 0xAD:  /* LODSW */
        case 0xAA:  /* STOSB */
        case 0xAB:  /* STOSW */
        case 0xA6:  /* CMPSB */
        case 0xA7:  /* CMPSW */
        case 0xAE:  /* SCASB */
        case 0xAF:  /* SCASW */
            emit_mov_reg(0, 4);
            emit_bl((uint32_t)&dinstr_exec_one);
            has_native_instrs = 0;
            offset++;
            instr_count++;
            goto block_done;

        /* ---- Conditional jumps (0x70-0x7F) ---- */
        case 0x70: case 0x71: case 0x72: case 0x73:
        case 0x74: case 0x75: case 0x76: case 0x77:
        case 0x78: case 0x79: case 0x7A: case 0x7B:
        case 0x7C: case 0x7D: case 0x7E: case 0x7F: {
            /* Jcond rel8 — condition evaluated at runtime via helper.
               The helper may modify cpu->ip if the jump is taken,
               so we must terminate the block to avoid executing
               wrong pre-translated instructions. */
            int8_t rel = (int8_t)mem[offset + 1];
            emit_mov_reg(0, 4);
            emit_mov_imm(1, opcode);
            emit_mov_imm(2, (uint8_t)rel);
            emit_bl((uint32_t)&dinstr_jcond_rel8);
            offset += 2;
            instr_count++;
            goto block_done;
        }

        /* ---- Opcodes 0x80-0x8F: ModR/M group and data-movement ---- */

        case 0x80: case 0x81: case 0x82: case 0x83:
            /* GRP1 Eb/Ib and Ev/Iv (ADD/OR/ADC/SBB/AND/SUB/XOR/CMP) — defer to interpreter */
            emit_mov_reg(0, 4);
            emit_bl((uint32_t)&dinstr_exec_one);
            has_native_instrs = 0;
            offset++;
            instr_count++;
            goto block_done;

        case 0x84:  /* TEST Gb, Eb */
            /* ModR/M-based — defer to interpreter */
            emit_mov_reg(0, 4);
            emit_bl((uint32_t)&dinstr_exec_one);
            has_native_instrs = 0;
            offset++;
            instr_count++;
            goto block_done;

        case 0x85:  /* TEST Gv, Ev */
            emit_mov_reg(0, 4);
            emit_bl((uint32_t)&dinstr_exec_one);
            has_native_instrs = 0;
            offset++;
            instr_count++;
            goto block_done;

        case 0x86:  /* XCHG Eb, Gb */
        case 0x87:  /* XCHG Ev, Gv */
        case 0x88:  /* MOV Eb, Gb */
        case 0x89:  /* MOV Ev, Gv */
        case 0x8A:  /* MOV Gb, Eb */
        case 0x8B:  /* MOV Gv, Ev */
            /* Full ModR/M r/m decode — defer to interpreter */
            emit_mov_reg(0, 4);
            emit_bl((uint32_t)&dinstr_exec_one);
            has_native_instrs = 0;
            offset++;
            instr_count++;
            goto block_done;

        case 0x8C:  /* MOV Ew, Sw */
            emit_mov_reg(0, 4);
            emit_bl((uint32_t)&dinstr_exec_one);
            has_native_instrs = 0;
            offset++;
            instr_count++;
            goto block_done;

        case 0x8D:  /* LEA Gv, M */
            emit_mov_reg(0, 4);
            emit_bl((uint32_t)&dinstr_exec_one);
            has_native_instrs = 0;
            offset++;
            instr_count++;
            goto block_done;

        case 0x8E:  /* MOV Sw, Ew */
            emit_mov_reg(0, 4);
            emit_bl((uint32_t)&dinstr_exec_one);
            has_native_instrs = 0;
            offset++;
            instr_count++;
            goto block_done;

        case 0x8F:  /* POP Ev */
            emit_mov_reg(0, 4);
            emit_bl((uint32_t)&dinstr_exec_one);
            has_native_instrs = 0;
            offset++;
            instr_count++;
            goto block_done;

        default:
            /* Unsupported opcode — bail out to interpreter */
            goto translate_fail;
        }

        if (emit32_failed) {
            DLOGI("translate_block: emit32 failed at opcode 0x%02X offset=%d", opcode, offset);
            goto translate_fail;
        }

        instr_count++;
    }

block_done:
    /* Check if code cache overflowed during translation */
    if (emit32_failed) {
        goto translate_fail;
    }

    /* Function epilogue */
    ARM_POP();  /* restore r4, r10, r11, pc */

    /* Store in block cache — but only if the block contains natively-translated
       instructions (not just interpreter fallbacks), since caching pure
       interpreter fallback blocks wastes cache entries. */
    if (has_native_instrs && block_cache_count < MAX_BLOCKS) {
        block_cache[block_cache_count].x86_pc = x86_pc;
        block_cache[block_cache_count].arm_offset = start_pos;
        block_cache[block_cache_count].code_len = code_cache_pos - start_pos;
        block_cache[block_cache_count].x86_len = offset;
        block_cache[block_cache_count].x86_end_ip = (uint16_t)(cpu->ip + offset);
        block_cache_count++;
    } else if (!has_native_instrs) {
        /* Block only contained interpreter fallbacks — return 0 so dynrec_exec
           falls back to interpreter directly (don't cache, don't execute). */
        code_cache_pos = start_pos;  /* discard emitted ARM code */
        return 0;
    }

    return instr_count;

translate_fail:
    /* Restore code cache position to discard any partially emitted garbage */
    code_cache_pos = start_pos;
    DLOGI("translate_block: failed, restored code_cache_pos to 0x%x", start_pos);
    return -1;
}

void dynrec_init(void) {
    /* Start with dynrec disabled - must be explicitly enabled */
    code_cache_pos = 0;
    block_cache_count = 0;
    memset(block_cache, 0, sizeof(block_cache));
    dynrec_enabled = 0;
    DLOGI("dynrec: initialized (disabled by default - enable via setspeed or nativeRun)");
}

void dynrec_reset(void) {
    if (!dynrec_enabled) return;
    if (code_cache_base) {
        code_cache_pos = 0;
    }
    block_cache_count = 0;
    memset(block_cache, 0, sizeof(block_cache));
    DLOGI("dynrec: reset");
}

void dynrec_enable(void) {
    if (dynrec_enabled) return;
    if (code_cache_base == NULL) {
        code_cache_base = mmap(NULL, CODE_CACHE_SIZE,
                               PROT_READ | PROT_WRITE | PROT_EXEC,
                               MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (code_cache_base == MAP_FAILED) {
            DLOGI("dynrec: mmap failed on enable");
            code_cache_base = NULL;
            return;
        }
    }
    code_cache_pos = 0;
    block_cache_count = 0;
    memset(block_cache, 0, sizeof(block_cache));
    dynrec_enabled = 1;
    DLOGI("dynrec: enabled, code_cache_base=%p", code_cache_base);
}

void dynrec_disable(void) {
    dynrec_enabled = 0;
    DLOGI("dynrec: disabled");
}

void dynrec_invalidate_range(uint32_t start, uint32_t len) {
    if (!dynrec_enabled) return;
    uint32_t end = start + len;
    for (int i = 0; i < block_cache_count; i++) {
        uint32_t block_end = block_cache[i].x86_pc + block_cache[i].x86_len;
        if (block_cache[i].x86_pc >= start && block_cache[i].x86_pc < end) {
            block_cache[i].x86_pc = 0;
        }
    }
}

int dynrec_exec(CPU_t* cpu, I8259_t* i8259, int max_instr) {
    if (!dynrec_enabled) return 0;

    int total_executed = 0;

    while (total_executed < max_instr) {
        /* Handle pending interrupts */
        uint8_t pending = i8259->irr & (~i8259->imr);
        if (!cpu->trap_toggle && cpu->ifl && pending) {
            cpu->hltstate = 0;
            cpu_intcall(cpu, i8259_nextintr(i8259));
            continue;
        }

        if (cpu->hltstate) {
            cpu_exec(cpu, 100);
            total_executed += 100;
            continue;
        }

        uint32_t x86_pc = (cpu->segregs[regcs] << 4) + cpu->ip;
        block_entry_t* block = find_block(x86_pc);

        if (block == NULL || block->x86_pc == 0) {
            int result = translate_block(cpu, x86_pc, MAX_INSTRUCTIONS);
            if (result < 0 || result == 0) {
                /* Can't translate or empty block — use interpreter */
                int batch = (max_instr - total_executed);
                if (batch > 1000) batch = 1000;
                DLOGI("dynrec_exec: falling back to cpu_exec with batch=%d at x86_pc=0x%06X", batch, x86_pc);
                cpu_exec(cpu, batch);
                dynrec_interpreter_instrs += batch;
                total_executed += batch;
                continue;
            }
            block = &block_cache[block_cache_count - 1];
            DLOGI("dynrec_exec: new block stored: x86_pc=0x%06X, x86_len=%u, arm_offset=%u", block->x86_pc, block->x86_len, block->arm_offset);
        }

        /* Flush data cache to ensure generated code is visible to instruction cache */
        __builtin___clear_cache(code_cache_base + block->arm_offset,
                                code_cache_base + block->arm_offset + block->code_len);

        /* Execute the translated block */
        typedef void (*block_func_t)(CPU_t* cpu);
        block_func_t func = (block_func_t)(code_cache_base + block->arm_offset);
        DLOGI("dynrec_exec: executing ARM block at offset=0x%06X, x86_len=%u, code_cache_base=%p, func=%p", block->arm_offset, block->x86_len, code_cache_base, (void*)func);

        /* Save IP before execution to detect if control flow modified it */
        uint16_t ip_before = cpu->ip;
        uint16_t expected_end_ip = (uint16_t)(cpu->ip + block->x86_len);

        /* Execute the ARM block — cpu is passed as R0 (first arg per AAPCS) */
        func(cpu);
        dynrec_native_blocks++;
        DLOGI("dynrec_exec: ARM block execution returned successfully (total native blocks: %llu)", (unsigned long long)dynrec_native_blocks);
        DLOGI("dynrec_exec: block x86_len=%u, total_executed=%d, max_instr=%d", block->x86_len, total_executed, max_instr);

        /* Guard against zero-length blocks (shouldn't happen, but prevents infinite loop) */
        if (block->x86_len == 0) {
            total_executed += 1;
            continue;
        }

        /* Advance IP for sequential execution. If a control-flow instruction
           (JMP/CALL/RET/INT/Jcond) modified cpu->ip, don't override it. */
        if (cpu->ip == ip_before) {
            cpu->ip = expected_end_ip;
        }

        /* Check if CPU was halted or interrupted during execution */
        if (cpu->hltstate || cpu->trap_toggle) {
            /* Let the interrupt check handle it */
            total_executed += block->x86_len;
            continue;
        }

        total_executed += block->x86_len;
        DLOGI("dynrec_exec: continue loop, total_executed=%d", total_executed);
    }

    return total_executed > 0 ? total_executed : max_instr;
}

/* Get statistics for diagnostics */
uint32_t dynrec_get_native_block_count(void) {
    return dynrec_native_blocks;
}
uint64_t dynrec_get_interpreter_instr_count(void) {
    return dynrec_interpreter_instrs;
}

#else  /* !__ARM_ARCH_5TE__ */

int dynrec_enabled = 0;

void dynrec_init(void) {}
void dynrec_reset(void) {}
int dynrec_exec(CPU_t* cpu, I8259_t* i8259, int max_instr) {
    (void)cpu; (void)i8259; (void)max_instr;
    return 0;
}
void dynrec_invalidate_range(uint32_t start, uint32_t len) {
    (void)start; (void)len;
}

#endif
