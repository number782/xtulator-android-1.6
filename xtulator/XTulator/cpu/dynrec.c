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
#define DBGLOG(...)
#endif

#ifdef __ANDROID__
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
static void crashlog(const char* fmt, ...) {
    FILE* f;
    va_list args;

    /*
	Gated on TRACE_FLAG_DIAG like every other /sdcard diagnostic. This was
	previously unconditional while DLOGI() respected the flags, which meant
	seven of the eighteen call sites fired an fopen/write/fflush/fclose
	cycle against the FUSE-backed /sdcard on the hot path — six of them
	per executed block. On the single-core 528MHz IS01 that alone starved
	the 8088 core and the PIT/PIC timers behind it. Turn the flag back on
	from the on-device trace menu when the dynrec log is actually needed.
    */
    if (!(trace_flags & TRACE_FLAG_DIAG)) return;

    f = fopen("/sdcard/xtulator_dynrec.log", "a");
    if (f) {
        va_start(args, fmt);
        vfprintf(f, fmt, args);
        va_end(args);
        fputc('\n', f);
        fclose(f);
    }
}
#else
static void crashlog(const char* fmt, ...) {}
#endif



#ifdef __ARM_ARCH_5TE__

/* On ARMv5TE Linux/Android, cacheflush is a syscall, not a libc function.
 * The Android NDK declares it in <unistd.h> as:
 *   extern int cacheflush(long start, long end, long flags);
 * We call it via syscall() to ensure proper ARM SWI convention.
 * However, the NDK declaration is sufficient — we just need to ensure
 * we don't conflict with it. The existing call site uses 0x3 for flags.
 *
 * Note: On older NDK versions this declaration may be missing, but r10e
 * includes it. If building with an older NDK, define our own. */
#ifdef __ANDROID__
/* If cacheflush is not declared by the system headers */
#ifndef _DECL_CACHEFLUSH
#define _DECL_CACHEFLUSH
/* The system already declares cacheflush in <unistd.h> */
#endif
#endif

/* Reduced from 512KB to 256KB to fit within IS01's memory constraints */
#define CODE_CACHE_SIZE    (256 * 1024)
#define MAX_BLOCKS         1024
#define MAX_INSTRUCTIONS   128

/*
	Flush the whole JIT when the code cache gets within this many bytes of
	full. Dead blocks are not compacted out of the code cache, so without a
	flush point the cache would eventually fill with unreachable code and
	emit32() would fail forever. Flushing everything is the simple correct
	answer: the worst case is that the blocks in flight get retranslated.
*/
#define CODE_CACHE_FLUSH_MARGIN (16 * 1024)

volatile int dynrec_enabled = 0;

/* ---- Code cache ---- */
static uint8_t* code_cache_base = NULL;
static uint32_t code_cache_pos = 0;

/* ---- Block cache ---- */
typedef struct {
    uint32_t x86_pc;      /* CS*16 + IP */
    uint32_t arm_offset;  /* offset in code cache */
    uint32_t code_len;    /* bytes of generated ARM code */
    /*
     * x86_len and instr_count are two DIFFERENT quantities and conflating
     * them was a real bug:
     *
     *   x86_len      BYTES of guest x86 code this block covers. This is the
     *                block's invalidation footprint (a write anywhere in
     *                [x86_pc, x86_pc + x86_len) makes the block stale) and
     *                the input to expected_end_ip. It must be the TRUE
     *                encoding length of every instruction in the block --
     *                see x86_instr_len() for why that is not just "1" for
     *                the interpreter-deferred opcodes.
     *
     *   instr_count  NUMBER of x86 instructions in the block. This is what
     *                dynrec_exec() accumulates into its "instructions
     *                executed" budget. Charging x86_len here made a block of
     *                five 1-byte MOVs count as 5 but a block of one 4-byte
     *                JZ count as 4, and a block containing a 6-byte
     *                ModR/M instruction count as 6.
     */
    uint32_t x86_len;
    uint32_t instr_count;
    uint32_t x86_end_ip;  /* expected IP after block (if no control flow) */
    uint8_t  valid;       /* 0 = free slot. Not x86_pc==0, which is a real address. */
} block_entry_t;

static block_entry_t block_cache[MAX_BLOCKS];
static int block_cache_count = 0;  /* high-water mark: slots [0, count) are allocated */

/* ---- Dynrec statistics (for diagnostics) ---- */
volatile uint32_t dynrec_native_blocks = 0;    /* count of ARM blocks executed natively */
volatile uint64_t dynrec_interpreter_instrs = 0;  /* count of interpreter-fallback instructions */

/* ---- ARM instruction emission ---- */

#define ARM_COND_AL  0xE0000000  /* condition: always */

static int emit32_failed = 0;

static void emit32(uint32_t instr) {
    if (emit32_failed) return;
    /* Guard against writing into the guard page (CODE_CACHE_SIZE is the
     * executable limit; the 4KB guard page starts at CODE_CACHE_SIZE). */
    if (code_cache_pos + 4 > CODE_CACHE_SIZE) {
        emit32_failed = 1;
        DLOGI("emit32: code cache overflow at pos=%u (limit=%u)", code_cache_pos, CODE_CACHE_SIZE);
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

/* MOVW Rd, #imm16 — load 16-bit immediate (ARMv5TE doesn't have MOVW).
 * Use MOV hi8; MOV Rd, Rd, LSL #4; MOV Rd, Rd, LSL #4; ORR Rd, Rd, #lo8.
 * This produces the correct little-endian 16-bit value (hi at bits 15-8, lo at bits 7-0).
 * ARM immediate encoding cannot represent 0x00HH00 (hi byte at bits 15-8)
 * as a single rotated immediate, so we must use register shifts.
 * Using two LSL #4 avoids a suspected encoding issue with LSL #8.
 * Encoding: 0xE1A00000 | (Rd<<12) | Rd | (shift_imm<<7) | (shift_type<<5) | Rm
 * For LSL #4: shift_imm=4 (at bits 11-7), shift_type=00 (LSL, at bits 6-5).
 * shift_imm=4 → (4<<7)=0x200; shift_type=00 → no bits at 6-5. NO 0x10! */
static void emit_movw(int Rd, uint16_t imm) {
    uint32_t lo = imm & 0xFF;
    uint32_t hi = (imm >> 8) & 0xFF;
    if (hi == 0) {
        emit32(ARM_COND_AL | 0x3A00000 | (Rd << 12) | lo);  /* MOV Rd, #lo */
    } else if (lo == 0) {
        /* MOV Rd, #hi; MOV Rd, Rd, LSL #4; MOV Rd, Rd, LSL #4 */
        emit32(ARM_COND_AL | 0x3A00000 | (Rd << 12) | hi);  /* MOV Rd, #hi */
        emit32(0xE1A00000 | (Rd << 12) | Rd | (4 << 7));  /* MOV Rd, Rd, LSL #4 */
        emit32(0xE1A00000 | (Rd << 12) | Rd | (4 << 7));  /* MOV Rd, Rd, LSL #4 */
    } else {
        /* MOV Rd, #hi; MOV Rd, Rd, LSL #4; MOV Rd, Rd, LSL #4; ORR Rd, Rd, #lo */
        emit32(ARM_COND_AL | 0x3A00000 | (Rd << 12) | hi);  /* MOV Rd, #hi */
        emit32(0xE1A00000 | (Rd << 12) | Rd | (4 << 7));  /* MOV Rd, Rd, LSL #4 */
        emit32(0xE1A00000 | (Rd << 12) | Rd | (4 << 7));  /* MOV Rd, Rd, LSL #4 */
        emit32(ARM_COND_AL | 0x3800000 | (Rd << 12) | (Rd << 16) | lo);  /* ORR Rd, Rd, #lo */
    }
}

/* LDR r0, =imm32 (literal pool) - not currently used */
static void emit_ldr_imm32(int Rd, uint32_t imm) {
    /* MOV + ORR approach for arbitrary 32-bit immediate.
     * Use register shifts (LSL #8, #16, #24) instead of broken immediate rotates. */
    uint8_t b0 = imm & 0xFF;
    uint8_t b1 = (imm >> 8) & 0xFF;
    uint8_t b2 = (imm >> 16) & 0xFF;
    uint8_t b3 = (imm >> 24) & 0xFF;
    if (b1 == 0 && b2 == 0 && b3 == 0) {
        emit32(ARM_COND_AL | 0x3A00000 | (Rd << 12) | b0);  /* MOV Rd, #b0 */
    } else if (b2 == 0 && b3 == 0) {
        emit32(ARM_COND_AL | 0x3A00000 | (Rd << 12) | b0);   /* MOV Rd, #b0 */
        if (b1 != 0) {
            emit32(ARM_COND_AL | 0xE1A00000 | (Rd << 12) | (Rd << 16) | (8 << 7));  /* MOV Rd, Rd, LSL #8 */
            emit32(ARM_COND_AL | 0x3800000 | (Rd << 12) | (Rd << 16) | b1);  /* ORR Rd, Rd, #b1 */
        }
    }
    /* For 3-4 byte immediates, would need additional shifts and ORRs */
}

/* BL to target — computes offset automatically at emit time.
 *
 * ======================================================================
 * THE INVARIANT BOTH PATHS SHARE — read this before changing either one
 * ======================================================================
 * ARM's `BL` (immediate) and `BLX <Rm>` set LR = PC = (address of the call
 * word) + 8, i.e. the word TWO positions on — NOT the word immediately
 * after it. The two forms do not differ in that respect, so emit_bl() must
 * produce the same shape for BOTH of them:
 *
 *     ... build R12 ...            (indirect path only)
 *     X+0:  BL  target  /  BLX R12 ; LR = X+8
 *     X+4:  B .-4  (0xEAFFFFFF)   ; spacing word, see below
 *     X+8:  <caller's next word>   ; <- LR points HERE
 *
 * The `B .-4` spacing word is required, not decoration. emit_bl() cannot
 * know whether more code follows: for the last helper call in a block the
 * next word the caller emits is the POP epilogue, and a call word with
 * LR = call+8 would return one word PAST it — skipping the epilogue,
 * leaving R4 and SP unrestored, and then executing whatever bytes follow
 * in the code cache as ARM instructions. Mid-block it is just as bad, in
 * the other direction: the word the caller emitted at call+4 is skipped.
 * The spacing word absorbs the +8 bias, so LR always lands on the caller's
 * next word whether that is more translated code or the epilogue. It is
 * never executed (the return address is one word past it) and branches to
 * the following word if it ever were.
 *
 * The two paths below therefore differ ONLY in HOW the target is reached
 * (a 24-bit direct offset vs. a full 32-bit address in R12), never in what
 * follows the call word. Keep them structurally parallel.
 */
static void emit_bl(uint32_t target_addr) {
    uint32_t pc = code_cache_pos;
    /* On ARM, PC reads as instruction_address + 8 during execution.
     * BL/BLX set LR = address of next instruction = (address of call) + 4.
     * So LR naturally points to the word immediately after the call.
     * No absorber word is needed or wanted. */
    uint32_t base_pc = (uint32_t)code_cache_base + pc;
    int32_t offset = (int32_t)(target_addr - (base_pc + 8)) / 4;

    if (offset > 8388607 || offset < -8388608) {
        /* Target is out of BL's ±32MB range (24-bit signed offset). This is the
         * normal case on the IS01: the code cache is mmap'd around 0x2d5b3000
         * while the helpers live in libxtulator.so around 0x42000000, ~355MB
         * apart, so *every* helper call lands here.
         *
         * Emit an indirect call that materialises the FULL 32-bit target into
         * R12 and uses BLX Rm. There is deliberately NO literal pool.
         *
         * Do NOT "optimise" this into `LDR R12,[PC,#n]` + `.word target`:
         * that is what this code used to do, and BLX's +8 bias put the pool
         * word exactly at LR, so the helper's own address was executed as ARM
         * code on every return from every helper call.
         *
         * Build the full 32-bit address using the same shift-based approach as
         * emit_movw: low 16 bits via emit_movw, then upper 16 bits via LSL #8
         * and ORR. This works for ANY 32-bit address, unlike ORR immediates
         * which only work for rotated-immediate encodable values. */
/* Build the full 32-bit address using emit_movw-style for low16,
          * then shift left 16 and add high16 via ORR. Works for ANY 32-bit value,
          * uses only R12 (no temp regs). */
         uint16_t low16 = target_addr & 0xFFFF;
         uint16_t high16 = (target_addr >> 16) & 0xFFFF;

         /* Build low16 in R12 using emit_movw logic */
         uint32_t lo = low16 & 0xFF;
         uint32_t hi = (low16 >> 8) & 0xFF;
         if (hi == 0) {
             emit32(ARM_COND_AL | 0x3A00000 | (12 << 12) | lo);  /* MOV R12, #lo */
         } else if (lo == 0) {
             emit32(ARM_COND_AL | 0x3A00000 | (12 << 12) | hi);  /* MOV R12, #hi */
             emit32(0xE1A00000 | (12 << 12) | 12 | (4 << 7));  /* MOV R12, R12, LSL #4 */
             emit32(0xE1A00000 | (12 << 12) | 12 | (4 << 7));  /* MOV R12, R12, LSL #4 (x2 = LSL #8) */
         } else {
             emit32(ARM_COND_AL | 0x3A00000 | (12 << 12) | hi);  /* MOV R12, #hi */
             emit32(0xE1A00000 | (12 << 12) | 12 | (4 << 7));  /* MOV R12, R12, LSL #4 */
             emit32(0xE1A00000 | (12 << 12) | 12 | (4 << 7));  /* MOV R12, R12, LSL #4 */
             emit32(ARM_COND_AL | 0x3800000 | (12 << 12) | (12 << 16) | lo);  /* ORR R12, R12, #lo */
         }

         if (high16 != 0) {
             /* Shift R12 left by 16 (LSL #8 twice) */
             emit32(0xE1A00000 | (12 << 12) | 12 | (8 << 7));  /* MOV R12, R12, LSL #8 */
             emit32(0xE1A00000 | (12 << 12) | 12 | (8 << 7));  /* MOV R12, R12, LSL #8 */

             /* Build high16 into R12 using ORR for first byte */
             uint32_t hi2 = (high16 >> 8) & 0xFF;
             uint32_t lo2 = high16 & 0xFF;
             if (hi2 != 0) {
                 emit32(ARM_COND_AL | 0x3800000 | (12 << 12) | (12 << 16) | hi2);  /* ORR R12, R12, #hi2 */
                 emit32(0xE1A00000 | (12 << 12) | 12 | (4 << 7));  /* MOV R12, R12, LSL #4 */
                 emit32(0xE1A00000 | (12 << 12) | 12 | (4 << 7));  /* MOV R12, R12, LSL #4 */
                 if (lo2 != 0) {
                     emit32(ARM_COND_AL | 0x3800000 | (12 << 12) | (12 << 16) | lo2);  /* ORR R12, R12, #lo2 */
                 }
             } else if (lo2 != 0) {
                 emit32(ARM_COND_AL | 0x3800000 | (12 << 12) | (12 << 16) | lo2);  /* ORR R12, R12, #lo2 */
             }
         }
        emit32(0xE12FFF3C);     /* BLX R12 — 0xE12FFF3C; NOT 0xE12FFF9C (that is MSR) */
    } else {
        emit32(0xEB000000 | (offset & 0xFFFFFF));  /* BL -- `offset` was computed
                                                       * from base_pc_plus_8, the
                                                       * address of THIS word, so it
                                                       * must not be shifted. */
    }
}

/* Placeholder macro for compatibility */
#define ARM_BL_PLACEHOLDER() (NULL)

/* B label (unconditional branch) */
static void emit_b_uncond(uint32_t* patch_slot) {
    emit32(ARM_COND_AL | 0x0A000000);  /* B placeholder */
    *patch_slot = code_cache_pos - 4;
}

/* SUBS r10, r10, #imm8 — base 0xE25AA000 (Rn=Rd=10); 0xE250A000 would be SUBS r10, r0, #imm */
static void emit_subs_imm(uint8_t imm) {
    emit32(0xE25AA000 | imm);
}

/* BNE target — patch later */
static uint32_t emit_bne_placeholder(void) {
    emit32(0x1A000000);  /* BNE placeholder (cond=0001=NE) */
    return code_cache_pos - 4;
}

/* Emit a call to dinstr_advance_ip() to advance cpu->ip by n bytes.
 *
 * The interpreter (cpu_exec) advances cpu->ip inside its own dispatch loop,
 * so the dinstr_* helpers were written assuming IP maintenance is someone
 * else's job. In generated code there is no dispatch loop, so the
 * translator has to do it explicitly — otherwise cpu->ip still points at
 * the first instruction of the block and any helper that reads IP
 * (dinstr_exec_one, jmp/call/ret/int) operates on the wrong address.
 *
 * Defined below the helper declarations since it references dinstr_advance_ip.
 */
static void emit_advance_ip(uint32_t n);

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

/* ---- Crash attribution ----
 * The generated blocks are anonymous mmap'd memory, so a fault inside one
 * shows up in the crash handler as a bare PC with no symbol and no way to
 * tell it apart from a fault in a helper. Publish the cache bounds and the
 * block currently executing so the handler can dump the offending ARM
 * instructions and the x86 PC that produced them. */

static volatile uint32_t g_cur_arm_off = 0xFFFFFFFFu; /* 0xFFFFFFFF = "none" */
static volatile uint32_t g_cur_code_len = 0;
static volatile uint32_t g_cur_x86_pc = 0;
/* The `cpu` pointer handed to func(cpu). Sampled from C, so it is always
 * trustworthy, and it is the discriminator: the block's prologue does
 * `mov r4, r0`, so if the fault-time R4 (available in the crash handler as
 * uc->uc_mcontext.arm_r4) differs from g_cur_cpu, R4 diverged from the cpu
 * pointer *inside* the block — an emitter/runtime bug. If they are equal the
 * block was entered with the correct cpu. There is no separate "entry R4"
 * sample: the block's R4 does not exist until the block starts. */
static volatile uint32_t g_cur_cpu = 0;
/* R4 as seen from dynrec_exec's OWN C frame, sampled immediately before
 * func(cpu). This is NOT the block's R4 (the block sets R4 itself in its
 * prologue). Its only use is the after-the-call comparison in dynrec_exec:
 * once func(cpu) returns, the block's epilogue `pop {r4,...}` has restored R4,
 * so a mismatch against this sample means the epilogue failed to restore R4. */
static volatile uint32_t g_cur_r4_in = 0;

/* Reading R4 needs real ARM codegen. dynrec.c is compiled for the host too
 * (the offline harness defines __ARM_ARCH_5TE__ to exercise the translator
 * on an x86 desktop), so gate on the actual architecture, not the guard. */
#if defined(__arm__) || defined(__thumb__)
static inline uint32_t dynrec_read_r4(void) {
    uint32_t r4;
    __asm__ __volatile__("mov %0, r4" : "=r"(r4));
    return r4;
}
#else
static inline uint32_t dynrec_read_r4(void) { return 0; }
#endif

int dynrec_get_code_cache(uintptr_t *base, uint32_t *size) {
    if (!base || !size) return 0;
    if (!code_cache_base) return 0;
    *base = (uintptr_t)code_cache_base;
    *size = CODE_CACHE_SIZE;
    return 1;
}

int dynrec_get_current_block(uint32_t *arm_off, uint32_t *code_len, uint32_t *x86_pc) {
    if (!arm_off || !code_len || !x86_pc) return 0;
    *arm_off = g_cur_arm_off;
    *code_len = g_cur_code_len;
    *x86_pc = g_cur_x86_pc;
    return 1;
}

/* *entry_cpu: the cpu pointer passed to func(cpu), sampled from C before the
 * call and cleared to 0 after it returns — 0 means no block is active.
 * *caller_r4: R4 of dynrec_exec's own C frame, sampled just before func(cpu).
 * It is NOT the block's R4 and must not be compared against fault-time R4;
 * compare *entry_cpu against fault-time R4 instead. */
int dynrec_get_current_entry(uint32_t *entry_cpu, uint32_t *caller_r4) {
    if (!entry_cpu || !caller_r4) return 0;
    *entry_cpu = g_cur_cpu;
    *caller_r4 = g_cur_r4_in;
    return 1;
}

/* Copies up to `count` guest bytes at physical address x86_pc (= (cs<<4)+ip
 * mod MEMORY_RANGE). Used by the native crash handler to report which x86
 * opcode was being emulated when a helper faulted. Mirrors the access guard
 * used by translate_block(): if the address is not direct-mapped RAM (NULL
 * page or an MMIO callback), fills with 0xFF and keeps going so the handler
 * can still show the instruction boundary it was working from. Returns the
 * number of bytes written (always == count on the RAM path). */
int dynrec_get_guest_bytes(uint32_t x86_pc, uint32_t count, uint8_t *out) {
    uint32_t i;
    if (count == 0 || out == NULL) return 0;
    for (i = 0; i < count; i++) {
        uint32_t addr = (x86_pc + i) & MEMORY_MASK;
        uint8_t *page = memory_mapRead[addr];
        if (page != NULL && memory_mapReadCallback[addr] == NULL) {
            out[i] = *page;
        } else {
            out[i] = 0xFF;
        }
    }
    return (int)i;
}

/* ---- Block cache ---- */

/* Number of generated blocks currently executing, i.e. depth of the
 * func(cpu) call in dynrec_exec(). flush_all_blocks() zeroes code_cache_pos
 * and wipes the block cache; doing that while a block is running would make
 * the running block's own code disappear underneath it and would let the
 * next translation overwrite the very instructions still executing. Nothing
 * calls it that way today, but that is an accident of the current call graph
 * rather than a guarantee, so the depth is tracked and the flush refused. */
static volatile int g_active_blocks = 0;

static void flush_all_blocks(void) {
    if (g_active_blocks > 0) {
        /* Refuse rather than silently reset code_cache_pos under a running
         * block: the correct outcome is that the cache fills up and translation
         * starts failing, which is recoverable, rather than live code being
         * overwritten mid-execution, which is not. */
        crashlog("FLUSH_REFUSED: flush_all_blocks() called with %d block(s) "
                 "active (code_cache_pos=%u) -- NOT flushing", g_active_blocks,
                 (unsigned)code_cache_pos);
        DLOGI("flush: refused, %d block(s) still executing", g_active_blocks);
        return;
    }
    code_cache_pos = 0;
    block_cache_count = 0;
    memset(block_cache, 0, sizeof(block_cache));
}

static block_entry_t* find_block(uint32_t x86_pc) {
    for (int i = 0; i < block_cache_count; i++) {
        if (block_cache[i].valid && block_cache[i].x86_pc == x86_pc) {
            return &block_cache[i];
        }
    }
    return NULL;
}

/*
	Append a block, reusing a slot freed by invalidation before growing.
	Returns NULL only if all MAX_BLOCKS slots are genuinely live, which the
	flush point in translate_block() should prevent.
*/
static block_entry_t* alloc_block_slot(void) {
    for (int i = 0; i < block_cache_count; i++) {
        if (!block_cache[i].valid) return &block_cache[i];
    }
    if (block_cache_count < MAX_BLOCKS) {
        return &block_cache[block_cache_count++];
    }
    return NULL;
}

static void invalidate_block(uint32_t x86_pc) {
    for (int i = 0; i < block_cache_count; i++) {
        if (block_cache[i].valid && block_cache[i].x86_pc == x86_pc) {
            block_cache[i].valid = 0;
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
extern int dinstr_mov_r8_imm8(CPU_t* cpu, uint8_t regnum, uint8_t imm);

extern int dinstr_jmp_rel8(CPU_t* cpu, int8_t rel);
extern int dinstr_jcond_rel8(CPU_t* cpu, uint8_t opcode, int8_t rel);
extern int dinstr_jmp_rel16(CPU_t* cpu, int16_t rel);
extern int dinstr_call_rel16(CPU_t* cpu, int16_t rel);
extern int dinstr_ret_near(CPU_t* cpu);
extern int dinstr_loop_rel8(CPU_t* cpu, uint8_t opcode, int8_t rel);
extern int dinstr_add_r16_imm(CPU_t* cpu, uint8_t regnum, uint16_t imm);
extern int dinstr_cmp_r16_imm(CPU_t* cpu, uint8_t regnum, uint16_t imm);
extern int dinstr_add_al_imm8(CPU_t* cpu, uint8_t imm);
extern int dinstr_add_ax_imm16(CPU_t* cpu, uint16_t imm);
extern int dinstr_pushf(CPU_t* cpu);
extern int dinstr_popf(CPU_t* cpu);
extern int dinstr_inc_r8(CPU_t* cpu, uint8_t regnum);
extern int dinstr_dec_r8(CPU_t* cpu, uint8_t regnum);
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
extern int dinstr_advance_ip(CPU_t* cpu, uint16_t n);
extern int dinstr_exec_one(CPU_t* cpu);

/* TEST AL, Ib and TEST AX, Iv */
extern int dinstr_test_al_imm8(CPU_t* cpu, uint8_t imm);
extern int dinstr_test_ax_imm16(CPU_t* cpu, uint16_t imm);

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

/*
 * On Android, libxtulator.so is built as a shared library (BUILD_SHARED_LIBRARY
 * forces -fPIC on all sources). With -fPIC, taking the address of a function at
 * *compile time* yields a link-time virtual address that the dynamic linker
 * adjusts at load time. But `&func` in C already reflects the *link-time*
 * address baked into the binary, NOT the runtime address after relocation.
 *
 * If we embed that link-time address into generated code, the BLX call lands
 * at the wrong offset when the .so is loaded at a different base. This caused
 * every dynrec helper call to hit cpu_exec's internal fetch loop instead of the
 * intended helper, leading to an infinite `B .-4` spin and a crash at the code
 * cache bounds.
 *
 * Fix: resolve every helper address at RUNTIME (in dynrec_enable, which runs
 * after the library is loaded) and store them in this table. emit_bl then uses
 * the stored runtime address, which is always correct regardless of PIC/PIE
 * relocation.
 */
struct dynrec_helpers_t {
    uint32_t dinstr_exec_one;
    uint32_t dinstr_advance_ip;
    uint32_t dinstr_nop;
    uint32_t dinstr_mov_r_i;
    uint32_t dinstr_inc_r16;
    uint32_t dinstr_dec_r16;
    uint32_t dinstr_push_r16;
    uint32_t dinstr_pop_r16;
    uint32_t dinstr_hlt;
    uint32_t dinstr_int;
    uint32_t dinstr_jmp_rel8;
    uint32_t dinstr_jcond_rel8;
    uint32_t dinstr_jmp_rel16;
    uint32_t dinstr_call_rel16;
    uint32_t dinstr_ret_near;
    uint32_t dinstr_add_r16_imm;
    uint32_t dinstr_cmp_r16_imm;
    uint32_t dinstr_add_al_imm8;
    uint32_t dinstr_add_ax_imm16;
    uint32_t dinstr_pushf;
    uint32_t dinstr_popf;
    uint32_t dinstr_inc_r8;
    uint32_t dinstr_dec_r8;
    uint32_t dinstr_push_es;
    uint32_t dinstr_pop_es;
    uint32_t dinstr_push_cs;
    uint32_t dinstr_pop_cs;
    uint32_t dinstr_push_ss;
    uint32_t dinstr_pop_ss;
    uint32_t dinstr_push_ds;
    uint32_t dinstr_pop_ds;
    uint32_t dinstr_sahf;
    uint32_t dinstr_lahf;
    uint32_t dinstr_test_al_imm8;
    uint32_t dinstr_test_ax_imm16;
    uint32_t dinstr_or_al_imm8;
    uint32_t dinstr_or_ax_imm16;
    uint32_t dinstr_xor_al_imm8;
    uint32_t dinstr_xor_ax_imm16;
    uint32_t dinstr_sub_al_imm8;
    uint32_t dinstr_sub_ax_imm16;
    uint32_t dinstr_cmp_al_imm8;
    uint32_t dinstr_cmp_ax_imm16;
    uint32_t dinstr_adc_al_imm8;
    uint32_t dinstr_adc_ax_imm16;
    uint32_t dinstr_sbb_al_imm8;
    uint32_t dinstr_sbb_ax_imm16;
    uint32_t dinstr_and_al_imm8;
    uint32_t dinstr_and_ax_imm16;
    uint32_t dinstr_mov_r8_imm8;
    uint32_t dinstr_loop_rel8;
    uint32_t dinstr_test_gb_eb;
    uint32_t dinstr_test_gv_ev;
    uint32_t dinstr_mov_ew_sw;
    uint32_t dinstr_lea_gv_m;
    uint32_t dinstr_mov_sw_ew;
    uint32_t dinstr_cmc;
    uint32_t dinstr_clc;
    uint32_t dinstr_stc;
    uint32_t dinstr_cli;
    uint32_t dinstr_sti;
    uint32_t dinstr_cld;
    uint32_t dinstr_std;
};
static struct dynrec_helpers_t g_helpers;

/* Emit a call to dinstr_advance_ip() to advance cpu->ip by n bytes.
 * See the forward declaration above for why this is needed. */
static void emit_advance_ip(uint32_t n) {
    if (n == 0) return;
    emit_mov_reg(0, 4);            /* R0 = cpu */
    /* A block can span MAX_INSTRUCTIONS * 3 bytes (>255), so this must be
     * a full 16-bit immediate, not emit_mov_imm. */
    emit_movw(1, (uint16_t)n);     /* R1 = byte count */
    emit_bl(g_helpers.dinstr_advance_ip);
}

/* Resolve all dynrec helper function addresses at runtime. Must be called
 * after the shared library is loaded (i.e. from dynrec_enable()). */
static void dynrec_resolve_helpers(void) {
    g_helpers.dinstr_exec_one    = (uint32_t)(uintptr_t)&dinstr_exec_one;
    g_helpers.dinstr_advance_ip  = (uint32_t)(uintptr_t)&dinstr_advance_ip;
    g_helpers.dinstr_nop         = (uint32_t)(uintptr_t)&dinstr_nop;
    g_helpers.dinstr_mov_r_i     = (uint32_t)(uintptr_t)&dinstr_mov_r_i;
    g_helpers.dinstr_inc_r16     = (uint32_t)(uintptr_t)&dinstr_inc_r16;
    g_helpers.dinstr_dec_r16     = (uint32_t)(uintptr_t)&dinstr_dec_r16;
    g_helpers.dinstr_push_r16    = (uint32_t)(uintptr_t)&dinstr_push_r16;
    g_helpers.dinstr_pop_r16     = (uint32_t)(uintptr_t)&dinstr_pop_r16;
    g_helpers.dinstr_hlt         = (uint32_t)(uintptr_t)&dinstr_hlt;
    g_helpers.dinstr_int         = (uint32_t)(uintptr_t)&dinstr_int;
    g_helpers.dinstr_jmp_rel8    = (uint32_t)(uintptr_t)&dinstr_jmp_rel8;
    g_helpers.dinstr_jcond_rel8  = (uint32_t)(uintptr_t)&dinstr_jcond_rel8;
    g_helpers.dinstr_jmp_rel16   = (uint32_t)(uintptr_t)&dinstr_jmp_rel16;
    g_helpers.dinstr_call_rel16  = (uint32_t)(uintptr_t)&dinstr_call_rel16;
    g_helpers.dinstr_ret_near    = (uint32_t)(uintptr_t)&dinstr_ret_near;
    g_helpers.dinstr_add_r16_imm = (uint32_t)(uintptr_t)&dinstr_add_r16_imm;
    g_helpers.dinstr_cmp_r16_imm = (uint32_t)(uintptr_t)&dinstr_cmp_r16_imm;
    g_helpers.dinstr_add_al_imm8 = (uint32_t)(uintptr_t)&dinstr_add_al_imm8;
    g_helpers.dinstr_add_ax_imm16 = (uint32_t)(uintptr_t)&dinstr_add_ax_imm16;
    g_helpers.dinstr_pushf       = (uint32_t)(uintptr_t)&dinstr_pushf;
    g_helpers.dinstr_popf        = (uint32_t)(uintptr_t)&dinstr_popf;
    g_helpers.dinstr_inc_r8      = (uint32_t)(uintptr_t)&dinstr_inc_r8;
    g_helpers.dinstr_dec_r8      = (uint32_t)(uintptr_t)&dinstr_dec_r8;
    g_helpers.dinstr_push_es     = (uint32_t)(uintptr_t)&dinstr_push_es;
    g_helpers.dinstr_pop_es      = (uint32_t)(uintptr_t)&dinstr_pop_es;
    g_helpers.dinstr_push_cs     = (uint32_t)(uintptr_t)&dinstr_push_cs;
    g_helpers.dinstr_pop_cs      = (uint32_t)(uintptr_t)&dinstr_pop_cs;
    g_helpers.dinstr_push_ss     = (uint32_t)(uintptr_t)&dinstr_push_ss;
    g_helpers.dinstr_pop_ss      = (uint32_t)(uintptr_t)&dinstr_pop_ss;
    g_helpers.dinstr_push_ds     = (uint32_t)(uintptr_t)&dinstr_push_ds;
    g_helpers.dinstr_pop_ds      = (uint32_t)(uintptr_t)&dinstr_pop_ds;
    g_helpers.dinstr_sahf        = (uint32_t)(uintptr_t)&dinstr_sahf;
    g_helpers.dinstr_lahf        = (uint32_t)(uintptr_t)&dinstr_lahf;
    g_helpers.dinstr_mov_r8_imm8  = (uint32_t)(uintptr_t)&dinstr_mov_r8_imm8;
    g_helpers.dinstr_loop_rel8    = (uint32_t)(uintptr_t)&dinstr_loop_rel8;
    g_helpers.dinstr_test_al_imm8 = (uint32_t)(uintptr_t)&dinstr_test_al_imm8;
    g_helpers.dinstr_test_ax_imm16 = (uint32_t)(uintptr_t)&dinstr_test_ax_imm16;
    g_helpers.dinstr_or_al_imm8  = (uint32_t)(uintptr_t)&dinstr_or_al_imm8;
    g_helpers.dinstr_or_ax_imm16 = (uint32_t)(uintptr_t)&dinstr_or_ax_imm16;
    g_helpers.dinstr_xor_al_imm8 = (uint32_t)(uintptr_t)&dinstr_xor_al_imm8;
    g_helpers.dinstr_xor_ax_imm16 = (uint32_t)(uintptr_t)&dinstr_xor_ax_imm16;
    g_helpers.dinstr_sub_al_imm8 = (uint32_t)(uintptr_t)&dinstr_sub_al_imm8;
    g_helpers.dinstr_sub_ax_imm16 = (uint32_t)(uintptr_t)&dinstr_sub_ax_imm16;
    g_helpers.dinstr_cmp_al_imm8 = (uint32_t)(uintptr_t)&dinstr_cmp_al_imm8;
    g_helpers.dinstr_cmp_ax_imm16 = (uint32_t)(uintptr_t)&dinstr_cmp_ax_imm16;
    g_helpers.dinstr_adc_al_imm8 = (uint32_t)(uintptr_t)&dinstr_adc_al_imm8;
    g_helpers.dinstr_adc_ax_imm16 = (uint32_t)(uintptr_t)&dinstr_adc_ax_imm16;
    g_helpers.dinstr_sbb_al_imm8 = (uint32_t)(uintptr_t)&dinstr_sbb_al_imm8;
    g_helpers.dinstr_sbb_ax_imm16 = (uint32_t)(uintptr_t)&dinstr_sbb_ax_imm16;
    g_helpers.dinstr_and_al_imm8 = (uint32_t)(uintptr_t)&dinstr_and_al_imm8;
    g_helpers.dinstr_and_ax_imm16 = (uint32_t)(uintptr_t)&dinstr_and_ax_imm16;
    g_helpers.dinstr_test_gb_eb  = (uint32_t)(uintptr_t)&dinstr_test_gb_eb;
    g_helpers.dinstr_test_gv_ev  = (uint32_t)(uintptr_t)&dinstr_test_gv_ev;
    g_helpers.dinstr_mov_ew_sw   = (uint32_t)(uintptr_t)&dinstr_mov_ew_sw;
    g_helpers.dinstr_lea_gv_m    = (uint32_t)(uintptr_t)&dinstr_lea_gv_m;
    g_helpers.dinstr_mov_sw_ew   = (uint32_t)(uintptr_t)&dinstr_mov_sw_ew;
    g_helpers.dinstr_cmc         = (uint32_t)(uintptr_t)&dinstr_cmc;
    g_helpers.dinstr_clc         = (uint32_t)(uintptr_t)&dinstr_clc;
    g_helpers.dinstr_stc         = (uint32_t)(uintptr_t)&dinstr_stc;
    g_helpers.dinstr_cli         = (uint32_t)(uintptr_t)&dinstr_cli;
    g_helpers.dinstr_sti         = (uint32_t)(uintptr_t)&dinstr_sti;
    g_helpers.dinstr_cld         = (uint32_t)(uintptr_t)&dinstr_cld;
    g_helpers.dinstr_std         = (uint32_t)(uintptr_t)&dinstr_std;

    DLOGI("dynrec: resolved helper addresses (dinstr_exec_one=0x%08X)",
          g_helpers.dinstr_exec_one);
}

/* ---- Block translation ---- */

/*
 * Length-only x86 instruction decoder.
 *
 * WHY THIS EXISTS. Every opcode the translator hands to the interpreter via
 * dinstr_exec_one() used to advance `offset` by exactly 1. That is wrong in
 * two places that matter:
 *
 *   - block->x86_len is the block's invalidation footprint (see
 *     dynrec_invalidate_range, which tests [x86_pc, x86_pc + x86_len) for
 *     overlap with a write). With a 1-byte charge, the ModR/M, SIB,
 *     displacement and immediate bytes of a deferred instruction lay OUTSIDE
 *     the block's range, so a self-modifying-code patch or a code load
 *     landing on any of them failed to invalidate the block and the stale
 *     translation was executed again.
 *   - block->x86_end_ip / expected_end_ip is cpu->ip + x86_len, so an
 *     IP-desync recovery would resume at the deferred instruction's own
 *     opcode byte and re-execute it forever.
 *
 * The alternative considered and rejected was a conservative footprint
 * (treat x86_len as a lower bound and additionally invalidate any block
 * within MAX_INSTRUCTIONS*6 bytes of a write). That fixes invalidation but
 * leaves x86_end_ip wrong and invalidates blocks that cannot possibly be
 * touched, so it degrades into a near-useless cache. Decoding the length is
 * the honest fix; the cost is bounded because the decode is a handful of
 * table lookups and happens once per instruction at translation time.
 *
 * CONTRACT: this function never UNDER-estimates. An over-estimate only widens
 * the invalidation footprint (safe: a spurious retranslation) and only
 * perturbs x86_end_ip, which is consulted solely on the IP-desync recovery
 * path. An unrecognised encoding therefore returns a generous upper bound
 * rather than guessing low. This decoder computes LENGTH ONLY -- it never
 * interprets operands, which is exactly the work dinstr_exec_one() is called
 * to do.
 */
static int x86_is_prefix(uint8_t b) {
    return b == 0x2E || b == 0x3E || b == 0x26 || b == 0x36 || b == 0x64 ||
           b == 0x65 || b == 0x66 || b == 0x67 || b == 0xF0 || b == 0xF2 ||
           b == 0xF3;
}

/* Length of the instruction at mem[0..avail). `avail` is how many bytes may be
 * read; the result is clamped to it so a truncated encoding at the top of the
 * address space cannot make the translator run off the mapped region. */
static uint32_t x86_instr_len(const uint8_t *mem, uint32_t avail) {
    uint32_t i = 0;
    int opsz16 = 0;   /* 0x66: operand size 16 -> 32, so Iv/rel widen to 32-bit */
    int adsz32 = 0;   /* 0x67: address size 16 -> 32: disp32, and the SIB byte
                         becomes possible (it does NOT exist in 16-bit mode) */
    uint8_t op, op2 = 0, m;
    int has_modrm = 0;
    uint32_t imm = 0;

    /* Legacy prefix run. Capped so a run of 0x90-adjacent junk cannot walk
     * the whole address space; the real limit is 4 prefixes on 386+. */
    while (i < avail && i < 4 && x86_is_prefix(mem[i])) {
        if (mem[i] == 0x66) opsz16 = 1;
        else if (mem[i] == 0x67) adsz32 = 1;
        i++;
    }
    if (i >= avail) return i;              /* truncated: prefix run only */

    op = mem[i++];

    if (op == 0x0F) {
        if (i >= avail) return i;
        op2 = mem[i++];
        if (op2 >= 0x80 && op2 <= 0x8F) {
            imm = opsz16 ? 4 : 2;          /* Jcc rel16/rel32 */
        } else if (op2 == 0x05 || op2 == 0x06 || op2 == 0x07 || op2 == 0x08 ||
                   op2 == 0x09 || op2 == 0x0B || op2 == 0x0E || op2 == 0x30 ||
                   op2 == 0x31 || op2 == 0x32 || op2 == 0x33 || op2 == 0x34 ||
                   op2 == 0x35 || op2 == 0x37 || op2 == 0x77 ||
                   (op2 >= 0xA0 && op2 <= 0xA1) ||   /* PUSH/POP FS */
                   (op2 >= 0xA8 && op2 <= 0xA9) ||   /* PUSH/POP GS */
                   (op2 >= 0xC8 && op2 <= 0xCF)) {   /* BSWAP */
            imm = 0;                        /* no operands at all */
        } else if (op2 == 0x0F) {
            has_modrm = 1; imm = 1;         /* 3DNow!: imm8 trails the ModR/M */
        } else if (op2 == 0x0D || op2 == 0x1F ||
                   (op2 >= 0x18 && op2 <= 0x1E)) {
            has_modrm = 1;                   /* PREFETCH / NOP Ev: no immediate */
        } else if (op2 == 0x70 || op2 == 0x71 || op2 == 0x72 || op2 == 0x73 ||
                   op2 == 0xBA || (op2 >= 0xC2 && op2 <= 0xC6)) {
            /* The 0F opcodes that do carry a trailing immediate (shift-group
             * imm8, BT/BTS/BTR/BTC imm8, CMPPS/CMPSS/... imm8). */
            has_modrm = 1;
            imm = 1;
        } else if (op2 == 0xA4 || op2 == 0xA5 ||
                   op2 == 0xAC || op2 == 0xAD) {
            /* SHLD/SHRD. The register form (mod=11) carries an imm8, the
             * memory form shifts by CL and carries none; charging the imm8
             * unconditionally over-counts the memory form by one byte, which
             * is the safe direction, and gets the register form right. */
            has_modrm = 1;
            imm = 1;
        } else if (op2 == 0xA0 || op2 == 0xA1 || op2 == 0xA2 ||
                   op2 == 0xA8 || op2 == 0xA9 || op2 == 0xAA) {
            /* PUSH/POP FS/GS and MOV to/from them use a moffs operand, not a
             * ModR/M: the address is a bare 16- or 32-bit offset. */
            imm = adsz32 ? 4 : 2;
        } else {
            /* Every other 0F opcode is ModR/M with no trailing immediate --
             * MOVZX/MOVSX, SETcc, CMOVcc, CMPXCHG, the SSE/MMX groups and so
             * on. Defaulting here (rather than charging an immediate) keeps
             * the length exact for the very common `0F 94 /0` SETcc and
             * `0F 44 /0` CMOVcc forms, which the interpreter fallback sees all
             * the time in 16-bit code. */
            has_modrm = 1;
        }
    } else if (op < 0x40 && (op & 7) <= 3) {
        has_modrm = 1;                      /* ADD/OR/ADC/SBB/AND/SUB/XOR/CMP Ev */
    } else {
        /*
         * One exhaustive switch. Every case is listed explicitly, including
         * the ones that take no operands, so that `default:` really does mean
         * "encoding not modelled here" and can return a conservative bound
         * without corrupting the known-correct opcodes.
         */
        switch (op) {
        /* --- 0x00-0x3F, non-ModR/M forms --- */
        case 0x04: case 0x0C: case 0x14: case 0x1C:
        case 0x24: case 0x2C: case 0x34: case 0x3C:
            imm = 1; break;                          /* AL, Ib */
        case 0x05: case 0x0D: case 0x15: case 0x1D:
        case 0x25: case 0x2D: case 0x35: case 0x3D:
            imm = opsz16 ? 4 : 2; break;             /* eAX, Iv */
        case 0x06: case 0x07: case 0x0E: case 0x16:
        case 0x17: case 0x1E: case 0x1F:
        case 0x26: case 0x27: case 0x2E: case 0x2F:
        case 0x37: case 0x3E: case 0x3F:
            break;                                   /* PUSH/POP Sreg, DAA family */
        /* --- 0x40-0x6F: INC/DEC r16, PUSH/POP r16, PUSHA/POPA --- */
        case 0x40: case 0x41: case 0x42: case 0x43: case 0x44: case 0x45:
        case 0x46: case 0x47: case 0x48: case 0x49: case 0x4A: case 0x4B:
        case 0x4C: case 0x4D: case 0x4E: case 0x4F:
        case 0x50: case 0x51: case 0x52: case 0x53: case 0x54: case 0x55:
        case 0x56: case 0x57: case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F:
        case 0x60: case 0x61: break;        /* INC/DEC/PUSH/POP r16, PUSHA/POPA */
        case 0x62: has_modrm = 1; break;              /* BOUND Gv,Ma */
        case 0x63: has_modrm = 1; break;              /* ARPL Ew,Gw */
        case 0x68: imm = opsz16 ? 4 : 2; break;       /* PUSH Iv */
        case 0x69: has_modrm = 1; imm = opsz16 ? 4 : 2; break;  /* IMUL Gv,Ev,Iv */
        case 0x6A: imm = 1; break;                    /* PUSH imm8 */
        case 0x6B: has_modrm = 1; imm = 1; break;     /* IMUL Gv,Ev,Ib */
        /* --- 0x70-0x7F: Jcc rel8 --- */
        case 0x70: case 0x71: case 0x72: case 0x73: case 0x74: case 0x75:
        case 0x76: case 0x77: case 0x78: case 0x79: case 0x7A: case 0x7B:
        case 0x7C: case 0x7D: case 0x7E: case 0x7F:
            imm = 1; break;
        /* --- 0x80-0x8F: group 1 and the ModR/M data-movement block --- */
        case 0x80: case 0x82: case 0x83:
            has_modrm = 1; imm = 1; break;           /* Eb/Ib, Ev/Ib (sign-ext) */
        case 0x81:
            has_modrm = 1; imm = opsz16 ? 4 : 2; break;   /* Ev/Iv */
        case 0x84: case 0x85: case 0x86: case 0x87: case 0x88: case 0x89:
        case 0x8A: case 0x8B: case 0x8C: case 0x8D: case 0x8E: case 0x8F:
            has_modrm = 1; break;                     /* ModR/M, no immediate */
        /* --- 0x90-0x9F --- */
        case 0x9A:                        /* CALL far ptr: seg:off */
            imm = (opsz16 ? 4 : 2) + (adsz32 ? 4 : 2);
            break;
        /* --- 0xA0-0xAF: moffs and the string instructions --- */
        case 0xA0: case 0xA1: case 0xA2: case 0xA3:
            imm = adsz32 ? 4 : 2; break;              /* AL/eAX, moffs */
        case 0xA4: case 0xA5: case 0xA6: case 0xA7:   /* MOVS/CMPS */
        case 0xAA: case 0xAB: case 0xAC: case 0xAD:   /* STOS/LODS */
        case 0xAE: case 0xAF:                         /* SCAS */
            break;                                    /* no operands */
        /* --- 0xB0-0xBF --- */
        case 0xB0: case 0xB1: case 0xB2: case 0xB3:
        case 0xB4: case 0xB5: case 0xB6: case 0xB7:
            imm = 1; break;                            /* MOV r8, Ib */
        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF:
            imm = opsz16 ? 4 : 2; break;               /* MOV r16/r32, Iv */
        /* --- 0xC0-0xCF --- */
        case 0xC0: case 0xC1: has_modrm = 1; imm = 1; break;   /* shift Grp2, imm8 */
        case 0xC2: imm = opsz16 ? 4 : 2; break;                 /* RET imm16 */
        case 0xC4: case 0xC5:                                     /* LES/LDS */
            /* The far POINTER is read from memory, not encoded inline, so
             * there is no immediate here -- unlike CALL/JMP FAR (0x9A/0xEA),
             * which do carry one. `les ax,[bx]` is 2 bytes. */
            has_modrm = 1;
            break;
        case 0xC6: has_modrm = 1; imm = 1; break;               /* MOV Eb, Ib */
        case 0xC7: has_modrm = 1; imm = opsz16 ? 4 : 2; break;  /* MOV Ev, Iv */
        case 0xC8: imm = 3; break;                               /* ENTER Iv, Ib */
        case 0xCA: case 0xCB: imm = opsz16 ? 4 : 2; break;       /* RETF imm16 */
        case 0xCD: imm = 1; break;                               /* INT imm8 */
        /* --- 0xD0-0xDF: shift group 2, AAM/AAD, x87 --- */
        case 0xD0: case 0xD1: case 0xD2: case 0xD3:
            has_modrm = 1; break;
        case 0xD4: case 0xD5: imm = 1; break;                    /* AAM/AAD Ib */
        case 0xD6: case 0xD7: break;                             /* SALC, XLAT */
        case 0x90: case 0x91: case 0x92: case 0x93: case 0x94: case 0x95:
        case 0x96: case 0x97: case 0x98:
        case 0x9B: case 0x9C: case 0x9D: case 0x9E: case 0x9F:
        case 0xF4: case 0xF5: case 0xF8: case 0xF9: case 0xFA:
        case 0xFB: case 0xFC: case 0xFD:
            break;                                    /* no operands */
        case 0xD8: case 0xD9: case 0xDA: case 0xDB:
        case 0xDC: case 0xDD: case 0xDE: case 0xDF:
            has_modrm = 1; break;                                 /* x87 escape */
        /* --- 0xE0-0xEF --- */
        case 0xE0: case 0xE1: case 0xE2: case 0xE3:
        case 0xE4: case 0xE5: case 0xE6: case 0xE7:
            imm = 1; break;                            /* loop/jcxz/IN/OUT Ib */
        case 0xE8: case 0xE9: imm = opsz16 ? 4 : 2; break;  /* CALL/JMP rel */
        case 0xEA:                        /* JMP far ptr: seg:off */
            imm = (opsz16 ? 4 : 2) + (adsz32 ? 4 : 2);
            break;
        case 0xEB: imm = 1; break;                               /* JMP rel8 */
        case 0xEC: case 0xED: case 0xEE: case 0xEF: break;        /* IN/OUT DX */
        /* --- 0xF0-0xFF --- */
        case 0xF6: case 0xF7:
            /* TEST's immediate depends on the reg field of the ModR/M, which
             * is only known after the ModR/M byte is read -- handled below. */
            has_modrm = 1;
            break;
        case 0xFE: case 0xFF:
            has_modrm = 1; break;                     /* INC/DEC/CALL/JMP/PUSH Ev */
        default:
            /*
             * Not modelled. Charge the longest immediate a 16-bit-mode
             * encoding can carry so the answer stays an UPPER bound: an
             * over-long charge only widens the invalidation footprint, which
             * is safe, whereas an under-charge would leave bytes of a live
             * instruction outside the block and let a patch to them go
             * unnoticed. In practice this is unreachable -- the translator
             * only calls the decoder for the opcode groups it defers to the
             * interpreter, and every one of those is a case above.
             */
            imm = 4;
            break;
        }
    }

    if (has_modrm) {
        if (i >= avail) return i;
        m = mem[i++];
        /*
         * The ModR/M reg field decides some immediates, and it has to be
         * consulted for BOTH mod=11 (register) and mod!=11 (memory) forms:
         * F6 /0 and F7 /0 are TEST with an immediate, and `F6 06 3E` is a
         * 5-byte memory-form TEST, not 4.
         */
        if (op == 0xF6 || op == 0xF7) {
            if (((m >> 3) & 7) <= 1)
                imm = (op == 0xF6) ? 1 : (opsz16 ? 4 : 2);
        }
        if ((m >> 6) != 3) {                  /* not register-direct */
            uint8_t mod = (uint8_t)(m >> 6);
            uint8_t rm = (uint8_t)(m & 7);
            if (adsz32) {
                /* 32-bit addressing (0x67). Here rm=100 really does mean "a
                 * SIB byte follows" and rm=101/mod=0 means disp32. */
                if (rm == 4) {                /* SIB byte follows */
                    uint8_t sib;
                    if (i >= avail) return i;
                    sib = mem[i++];
                    if ((sib & 7) == 5) i += 4;       /* disp32, no base */
                } else if (rm == 5 && mod == 0) {
                    i += 4;                              /* disp32, no base */
                }
                if (mod == 1)      i += 1;
                else if (mod == 2) i += 4;
            } else {
                /*
                 * 16-bit addressing. THE SIB BYTE DOES NOT EXIST HERE: rm=100
                 * is [SI], rm=101 is [DI] and rm=110 is [BP]. Only [BP] with
                 * mod=0 needs a displacement (disp16). Decoding rm=100 as a
                 * SIB in 16-bit mode -- the obvious mistake -- inflates or
                 * deflates the length of every [SI+..] instruction.
                 */
                if (rm == 6 && mod == 0) i += 2;        /* [BP] -> disp16 */
                else if (mod == 1)      i += 1;
                else if (mod == 2)      i += 2;
            }
        } else if (op >= 0xD8 && op <= 0xDF) {
            /* Register-direct x87 escapes with reg >= 4 take an imm8. */
            if (((m >> 3) & 7) >= 4) imm = 1;
        }
    }

    i += imm;
    if (i > avail) i = avail;
    if (i == 0) i = 1;
    return i;
}

static int translate_block(CPU_t* cpu, uint32_t x86_pc, int max_instr) {
    uint8_t* mem = memory_mapRead[x86_pc & MEMORY_MASK];
    if (mem == NULL || memory_mapReadCallback[x86_pc & MEMORY_MASK] != NULL) {
        return -1;  /* Can't translate MMIO */
    }

    uint32_t start_pos = code_cache_pos;

    /*
	Flush before we run out of code cache. Dead blocks leave unreachable
	code behind, so without this the cache would fill up permanently and
	every later translation would fail. This also guarantees the block
	cache has room for a fresh block.
    */
    if (code_cache_pos + CODE_CACHE_FLUSH_MARGIN >= CODE_CACHE_SIZE) {
        flush_all_blocks();
        start_pos = 0;
    }

    /* Check if block cache is full */
    if (block_cache_count >= MAX_BLOCKS) {
        return -1;
    }

    /* Reset emit overflow flag for this translation */
    emit32_failed = 0;

    /* Function prologue: save callee-saved registers and save cpu pointer */
    ARM_PUSH();  /* save r4, r10, r11, lr */
    emit_mov_reg(4, 0);  /* R4 = cpu (first argument) — preserve across all helper calls */

    int instr_count = 0;
    int offset = 0;
    /* Bytes of already-emitted instructions not yet folded into cpu->ip.
     * Flushed at the top of each iteration so that any helper which reads
     * cpu->ip (dinstr_exec_one, jmp/call/ret/int) sees the address of the
     * instruction it is about to run rather than the block's first byte. */
    uint32_t pending_ip = 0;

    while (instr_count < max_instr) {
        /* Commit the IP advance owed by previously emitted instructions. */
        emit_advance_ip(pending_ip);
        pending_ip = 0;

        uint32_t prev_offset = (uint32_t)offset;
        uint8_t opcode = mem[offset];

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
            /* A prefix is not an instruction on its own — it modifies how
             * the FOLLOWING opcode decodes (operand size, segment, REP
             * count). Advancing past it and translating the next opcode
             * natively would silently drop that effect, e.g. "66 89 06"
             * (16-bit MOV) being run as an 8-bit MOV. So hand the whole
             * prefixed instruction to the interpreter, which re-reads the
             * prefix itself. */
            emit_mov_reg(0, 4);
            emit_bl(g_helpers.dinstr_exec_one);
            /* Charge the instruction its REAL byte length, not 1:
             * `offset` becomes block->x86_len, the block's invalidation
             * footprint and the input to expected_end_ip. See
             * x86_instr_len() for why 1 was wrong and what it costs. */
            offset += (int)x86_instr_len(&mem[offset],
                              MEMORY_RANGE - ((x86_pc + offset) & MEMORY_MASK));
            instr_count++;
            goto block_done;

        case 0x0F:  /* Two-byte opcode escape (0F xx) — defer to interpreter */
            emit_mov_reg(0, 4);
            emit_bl(g_helpers.dinstr_exec_one);
            /* Charge the instruction its REAL byte length, not 1:
             * `offset` becomes block->x86_len, the block's invalidation
             * footprint and the input to expected_end_ip. See
             * x86_instr_len() for why 1 was wrong and what it costs. */
            offset += (int)x86_instr_len(&mem[offset],
                              MEMORY_RANGE - ((x86_pc + offset) & MEMORY_MASK));
            instr_count++;
            goto block_done;
        /* ---- ADD opcodes (0x00-0x05) ---- */
        case 0x00: case 0x01: case 0x02: case 0x03:
            /* ADD Eb,Gb / ADD Ev,Gv / ADD Gb,Eb / ADD Gv,Ev — ModR/M forms */
            emit_mov_reg(0, 4);
            emit_bl(g_helpers.dinstr_exec_one);
            /* Charge the instruction its REAL byte length, not 1:
             * `offset` becomes block->x86_len, the block's invalidation
             * footprint and the input to expected_end_ip. See
             * x86_instr_len() for why 1 was wrong and what it costs. */
            offset += (int)x86_instr_len(&mem[offset],
                              MEMORY_RANGE - ((x86_pc + offset) & MEMORY_MASK));
            instr_count++;
            goto block_done;

        case 0x04:  /* ADD AL, Ib */
            emit_mov_reg(0, 4);  /* R0 = cpu (saved in R4) */
            emit_mov_imm(1, mem[offset + 1]);  /* R1 = imm8 */
            emit_bl(g_helpers.dinstr_add_al_imm8);
            offset += 2;
            break;

        case 0x05:  /* ADD AX, Iv */
            {
                uint16_t imm = mem[offset + 1] | (mem[offset + 2] << 8);
                emit_mov_reg(0, 4);  /* R0 = cpu */
                emit_movw(1, imm);  /* R1 = imm16 */
                emit_bl(g_helpers.dinstr_add_ax_imm16);
            }
            offset += 3;
            break;

        /* ---- SUB opcodes (0x28-0x2D) ---- */
        case 0x28: case 0x29: case 0x2A: case 0x2B:
            /* SUB Eb,Gb / SUB Ev,Gv / SUB Gb,Eb / SUB Gv,Ev — ModR/M forms */
            emit_mov_reg(0, 4);
            emit_bl(g_helpers.dinstr_exec_one);
            /* Charge the instruction its REAL byte length, not 1:
             * `offset` becomes block->x86_len, the block's invalidation
             * footprint and the input to expected_end_ip. See
             * x86_instr_len() for why 1 was wrong and what it costs. */
            offset += (int)x86_instr_len(&mem[offset],
                              MEMORY_RANGE - ((x86_pc + offset) & MEMORY_MASK));
            instr_count++;
            goto block_done;

        case 0x2C:  /* SUB AL, Ib */
            emit_mov_reg(0, 4);
            emit_mov_imm(1, mem[offset + 1]);
            emit_bl(g_helpers.dinstr_sub_al_imm8);
            offset += 2;
            break;

        case 0x2D:  /* SUB AX, Iv */
            {
                uint16_t imm = mem[offset + 1] | (mem[offset + 2] << 8);
                emit_mov_reg(0, 4);
                emit_movw(1, imm);
                emit_bl(g_helpers.dinstr_sub_ax_imm16);
            }
            offset += 3;
            break;

        /* ---- CMP opcodes (0x38-0x3D) ---- */
        case 0x38: case 0x39: case 0x3A: case 0x3B:
            /* CMP Eb,Gb / CMP Ev,Gv / CMP Gb,Eb / CMP Gv,Ev — ModR/M forms */
            emit_mov_reg(0, 4);
            emit_bl(g_helpers.dinstr_exec_one);
            /* Charge the instruction its REAL byte length, not 1:
             * `offset` becomes block->x86_len, the block's invalidation
             * footprint and the input to expected_end_ip. See
             * x86_instr_len() for why 1 was wrong and what it costs. */
            offset += (int)x86_instr_len(&mem[offset],
                              MEMORY_RANGE - ((x86_pc + offset) & MEMORY_MASK));
            instr_count++;
            goto block_done;

        case 0x3C:  /* CMP AL, Ib */
            emit_mov_reg(0, 4);
            emit_mov_imm(1, mem[offset + 1]);
            emit_bl(g_helpers.dinstr_cmp_al_imm8);
            offset += 2;
            break;

        case 0x3D:  /* CMP AX, Iv */
            {
                uint16_t imm = mem[offset + 1] | (mem[offset + 2] << 8);
                emit_mov_reg(0, 4);
                emit_movw(1, imm);
                emit_bl(g_helpers.dinstr_cmp_ax_imm16);
            }
            offset += 3;
            break;

        case 0x90:  /* NOP */
            emit_mov_reg(0, 4);
            emit_bl(g_helpers.dinstr_nop);
            offset++;
            break;

        case 0xB8: case 0xB9: case 0xBA: case 0xBB:
        case 0xBC: case 0xBD: case 0xBE: case 0xBF: {
            /* MOV reg, imm16 */
            uint8_t regnum = opcode - 0xB8;
            emit_mov_reg(0, 4);  /* R0 = cpu */
            emit_movw(1, regnum);
            emit_movw(2, mem[offset + 1] | (mem[offset + 2] << 8));
            emit_bl(g_helpers.dinstr_mov_r_i);
            offset += 3;
            break;
        }

        case 0x40: case 0x41: case 0x42: case 0x43:
        case 0x44: case 0x45: case 0x46: case 0x47:
            /* INC eXX */
            emit_mov_reg(0, 4);
            emit_movw(1, (uint16_t)(opcode - 0x40));
            emit_bl(g_helpers.dinstr_inc_r16);
            offset++;
            break;

        case 0x48: case 0x49: case 0x4A: case 0x4B:
        case 0x4C: case 0x4D: case 0x4E: case 0x4F:
            /* DEC eXX */
            emit_mov_reg(0, 4);
            emit_movw(1, (uint16_t)(opcode - 0x48));
            emit_bl(g_helpers.dinstr_dec_r16);
            offset++;
            break;

        case 0x50: case 0x51: case 0x52: case 0x53:
        case 0x54: case 0x55: case 0x56: case 0x57:
            /* PUSH eXX */
            emit_mov_reg(0, 4);
            emit_movw(1, (uint16_t)(opcode - 0x50));
            emit_bl(g_helpers.dinstr_push_r16);
            offset++;
            break;

        case 0x58: case 0x59: case 0x5A: case 0x5B:
        case 0x5C: case 0x5D: case 0x5E: case 0x5F:
            /* POP eXX */
            emit_mov_reg(0, 4);
            emit_movw(1, (uint16_t)(opcode - 0x58));
            emit_bl(g_helpers.dinstr_pop_r16);
            offset++;
            break;

        case 0xB0: case 0xB1: case 0xB2: case 0xB3:
        case 0xB4: case 0xB5: case 0xB6: case 0xB7:
            /* MOV r8, Ib */
            emit_mov_reg(0, 4);
            emit_movw(1, (uint16_t)(opcode - 0xB0));
            emit_mov_imm(2, mem[offset + 1]);
            emit_bl(g_helpers.dinstr_mov_r8_imm8);
            offset += 2;
            pending_ip += 2;
            instr_count++;
            break;

        case 0x9C:  /* PUSHF */
            emit_mov_reg(0, 4);
            emit_bl(g_helpers.dinstr_pushf);
            offset++;
            break;

        case 0x9D:  /* POPF */
            emit_mov_reg(0, 4);
            emit_bl(g_helpers.dinstr_popf);
            offset++;
            break;

        case 0xC3:  /* RET (near) */
            emit_mov_reg(0, 4);
            emit_bl(g_helpers.dinstr_ret_near);
            offset++;
            instr_count++;
            goto block_done;

        case 0xCD:  /* INT imm8 */
            emit_mov_reg(0, 4);
            emit_mov_imm(1, mem[offset + 1]);
            emit_bl(g_helpers.dinstr_int);
            offset += 2;
            instr_count++;
            goto block_done;

        case 0xE8:  /* CALL rel16 */
            {
                int16_t rel = mem[offset + 1] | (mem[offset + 2] << 8);
                emit_mov_reg(0, 4);
                emit_movw(1, (uint16_t)rel);
                emit_bl(g_helpers.dinstr_call_rel16);
            }
            offset += 3;
            instr_count++;
            goto block_done;

        case 0xE9:  /* JMP rel16 */
            {
                int16_t rel = mem[offset + 1] | (mem[offset + 2] << 8);
                emit_mov_reg(0, 4);
                emit_movw(1, (uint16_t)rel);
                emit_bl(g_helpers.dinstr_jmp_rel16);
            }
            offset += 3;
            instr_count++;
            goto block_done;

        case 0xEB:  /* JMP rel8 */
            emit_mov_reg(0, 4);
            emit_mov_imm(1, (uint8_t)mem[offset + 1]);
            emit_bl(g_helpers.dinstr_jmp_rel8);
            offset += 2;
            instr_count++;
            goto block_done;

        case 0xE0: case 0xE1: case 0xE2: case 0xE3:
            /* LOOPNE/LOOPZ/LOOP/JCXZ rel8 */
            emit_mov_reg(0, 4);
            emit_mov_imm(1, opcode);
            emit_mov_imm(2, (uint8_t)mem[offset + 1]);
            emit_bl(g_helpers.dinstr_loop_rel8);
            offset += 2;
            instr_count++;
            goto block_done;

        case 0xF4:  /* HLT */
            emit_mov_reg(0, 4);
            emit_bl(g_helpers.dinstr_hlt);
            offset++;
            /* dinstr_hlt only sets hltstate; unlike the other terminators
             * it does not advance cpu->ip past its own byte, so account for
             * that byte here or the IP will stay parked on the HLT. */
            pending_ip += 1;
            instr_count++;
            goto block_done;

        /* ---- Segment register operations ---- */
        case 0x06:  /* PUSH ES */
            emit_mov_reg(0, 4);
            emit_bl(g_helpers.dinstr_push_es);
            offset++;
            break;

        case 0x07:  /* POP ES */
            emit_mov_reg(0, 4);
            emit_bl(g_helpers.dinstr_pop_es);
            offset++;
            break;

        /* ---- OR instructions ---- */
        case 0x08:  /* OR Eb,Gb */
        case 0x09:  /* OR Ev,Gv */
        case 0x0A:  /* OR Gb,Eb */
        case 0x0B:  /* OR Gv,Ev */
            emit_mov_reg(0, 4);
            emit_bl(g_helpers.dinstr_exec_one);
            /* Charge the instruction its REAL byte length, not 1:
             * `offset` becomes block->x86_len, the block's invalidation
             * footprint and the input to expected_end_ip. See
             * x86_instr_len() for why 1 was wrong and what it costs. */
            offset += (int)x86_instr_len(&mem[offset],
                              MEMORY_RANGE - ((x86_pc + offset) & MEMORY_MASK));
            instr_count++;
            goto block_done;

        case 0x0C:  /* OR AL,Ib */
            emit_mov_reg(0, 4);
            emit_mov_imm(1, mem[offset + 1]);
            emit_bl(g_helpers.dinstr_or_al_imm8);
            offset += 2;
            break;

        case 0x0D:  /* OR AX,Iv */
            emit_mov_reg(0, 4);
            emit_movw(1, mem[offset + 1] | (mem[offset + 2] << 8));
            emit_bl(g_helpers.dinstr_or_ax_imm16);
            offset += 3;
            break;

        case 0x0E:  /* PUSH CS */
            emit_mov_reg(0, 4);
            emit_bl(g_helpers.dinstr_push_cs);
            offset++;
            break;

        /* ---- ADC instructions (0x10-0x15) ---- */
        case 0x10:  /* ADC Eb,Gb */
        case 0x11:  /* ADC Ev,Gv */
        case 0x12:  /* ADC Gb,Eb */
        case 0x13:  /* ADC Gv,Ev */
            emit_mov_reg(0, 4);
            emit_bl(g_helpers.dinstr_exec_one);
            /* Charge the instruction its REAL byte length, not 1:
             * `offset` becomes block->x86_len, the block's invalidation
             * footprint and the input to expected_end_ip. See
             * x86_instr_len() for why 1 was wrong and what it costs. */
            offset += (int)x86_instr_len(&mem[offset],
                              MEMORY_RANGE - ((x86_pc + offset) & MEMORY_MASK));
            instr_count++;
            goto block_done;

        case 0x14:  /* ADC AL,Ib */
            emit_mov_reg(0, 4);
            emit_mov_imm(1, mem[offset + 1]);
            emit_bl(g_helpers.dinstr_adc_al_imm8);
            offset += 2;
            break;

        case 0x15:  /* ADC AX,Iv */
            emit_mov_reg(0, 4);
            emit_movw(1, mem[offset + 1] | (mem[offset + 2] << 8));
            emit_bl(g_helpers.dinstr_adc_ax_imm16);
            offset += 3;
            break;

        /* ---- SBB instructions (0x18-0x1D) ---- */
        case 0x18:  /* SBB Eb,Gb */
        case 0x19:  /* SBB Ev,Gv */
        case 0x1A:  /* SBB Gb,Eb */
        case 0x1B:  /* SBB Gv,Ev */
            emit_mov_reg(0, 4);
            emit_bl(g_helpers.dinstr_exec_one);
            /* Charge the instruction its REAL byte length, not 1:
             * `offset` becomes block->x86_len, the block's invalidation
             * footprint and the input to expected_end_ip. See
             * x86_instr_len() for why 1 was wrong and what it costs. */
            offset += (int)x86_instr_len(&mem[offset],
                              MEMORY_RANGE - ((x86_pc + offset) & MEMORY_MASK));
            instr_count++;
            goto block_done;

        case 0x1C:  /* SBB AL,Ib */
            emit_mov_reg(0, 4);
            emit_mov_imm(1, mem[offset + 1]);
            emit_bl(g_helpers.dinstr_sbb_al_imm8);
            offset += 2;
            break;

        case 0x1D:  /* SBB AX,Iv */
            emit_mov_reg(0, 4);
            emit_movw(1, mem[offset + 1] | (mem[offset + 2] << 8));
            emit_bl(g_helpers.dinstr_sbb_ax_imm16);
            offset += 3;
            break;

        case 0x16:  /* PUSH SS */
            emit_mov_reg(0, 4);
            emit_bl(g_helpers.dinstr_push_ss);
            offset++;
            break;

        case 0x17:  /* POP SS */
            emit_mov_reg(0, 4);
            emit_bl(g_helpers.dinstr_pop_ss);
            offset++;
            break;

        case 0x1E:  /* PUSH DS */
            emit_mov_reg(0, 4);
            emit_bl(g_helpers.dinstr_push_ds);
            offset++;
            break;

        case 0x1F:  /* POP DS */
            emit_mov_reg(0, 4);
            emit_bl(g_helpers.dinstr_pop_ds);
            offset++;
            break;

        /* ---- XOR instructions ---- */
        case 0x30:  /* XOR Eb,Gb */
        case 0x31:  /* XOR Ev,Gv */
        case 0x32:  /* XOR Gb,Eb */
        case 0x33:  /* XOR Gv,Ev */
            emit_mov_reg(0, 4);
            emit_bl(g_helpers.dinstr_exec_one);
            /* Charge the instruction its REAL byte length, not 1:
             * `offset` becomes block->x86_len, the block's invalidation
             * footprint and the input to expected_end_ip. See
             * x86_instr_len() for why 1 was wrong and what it costs. */
            offset += (int)x86_instr_len(&mem[offset],
                              MEMORY_RANGE - ((x86_pc + offset) & MEMORY_MASK));
            instr_count++;
            goto block_done;

        case 0x34:  /* XOR AL,Ib */
            emit_mov_reg(0, 4);
            emit_mov_imm(1, mem[offset + 1]);
            emit_bl(g_helpers.dinstr_xor_al_imm8);
            offset += 2;
            break;

        case 0x35:  /* XOR AX,Iv */
            emit_mov_reg(0, 4);
            emit_movw(1, mem[offset + 1] | (mem[offset + 2] << 8));
            emit_bl(g_helpers.dinstr_xor_ax_imm16);
            offset += 3;
            break;

        /* ---- AND opcodes (0x20-0x25) ---- */
        case 0x20: case 0x21: case 0x22: case 0x23:
            /* AND Eb,Gb / AND Ev,Gv / AND Gb,Eb / AND Gv,Ev — ModR/M forms */
            emit_mov_reg(0, 4);
            emit_bl(g_helpers.dinstr_exec_one);
            /* Charge the instruction its REAL byte length, not 1:
             * `offset` becomes block->x86_len, the block's invalidation
             * footprint and the input to expected_end_ip. See
             * x86_instr_len() for why 1 was wrong and what it costs. */
            offset += (int)x86_instr_len(&mem[offset],
                              MEMORY_RANGE - ((x86_pc + offset) & MEMORY_MASK));
            instr_count++;
            goto block_done;

        case 0x24:  /* AND AL, Ib */
            emit_mov_reg(0, 4);
            emit_mov_imm(1, mem[offset + 1]);
            emit_bl(g_helpers.dinstr_and_al_imm8);
            offset += 2;
            break;

        case 0x25:  /* AND AX, Iv */
            {
                uint16_t imm = mem[offset + 1] | (mem[offset + 2] << 8);
                emit_mov_reg(0, 4);
                emit_movw(1, imm);
                emit_bl(g_helpers.dinstr_and_ax_imm16);
            }
            offset += 3;
            break;

        /* ---- TEST AL/AX with immediate (0xA8-0xA9) ---- */
        case 0xA8:  /* TEST AL, Ib */
            emit_mov_reg(0, 4);
            emit_mov_imm(1, mem[offset + 1]);
            emit_bl(g_helpers.dinstr_test_al_imm8);
            offset += 2;
            break;

        case 0xA9:  /* TEST AX, Iv */
            emit_mov_reg(0, 4);
            emit_movw(1, mem[offset + 1] | (mem[offset + 2] << 8));
            emit_bl(g_helpers.dinstr_test_ax_imm16);
            offset += 3;
            break;

        /* ---- SAHF/LAHF ---- */
        case 0x9E:  /* SAHF */
            emit_mov_reg(0, 4);
            emit_bl(g_helpers.dinstr_sahf);
            offset++;
            break;

        case 0x9F:  /* LAHF */
            emit_mov_reg(0, 4);
            emit_bl(g_helpers.dinstr_lahf);
            offset++;
            break;

        /* ---- Flag operations ---- */
        case 0xF0:  /* LOCK prefix — skip, no operation */
            offset++;
            break;

        case 0xF5:  /* CMC */
            emit_mov_reg(0, 4);
            emit_bl(g_helpers.dinstr_cmc);
            offset++;
            break;

        case 0xF8:  /* CLC */
            emit_mov_reg(0, 4);
            emit_bl(g_helpers.dinstr_clc);
            offset++;
            break;

        case 0xF9:  /* STC */
            emit_mov_reg(0, 4);
            emit_bl(g_helpers.dinstr_stc);
            offset++;
            break;

        case 0xFA:  /* CLI */
            emit_mov_reg(0, 4);
            emit_bl(g_helpers.dinstr_cli);
            offset++;
            break;

        case 0xFB:  /* STI */
            emit_mov_reg(0, 4);
            emit_bl(g_helpers.dinstr_sti);
            offset++;
            break;

        case 0xFC:  /* CLD */
            emit_mov_reg(0, 4);
            emit_bl(g_helpers.dinstr_cld);
            offset++;
            break;

        case 0xFD:  /* STD */
            emit_mov_reg(0, 4);
            emit_bl(g_helpers.dinstr_std);
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
            emit_bl(g_helpers.dinstr_exec_one);
            /* Charge the instruction its REAL byte length, not 1:
             * `offset` becomes block->x86_len, the block's invalidation
             * footprint and the input to expected_end_ip. See
             * x86_instr_len() for why 1 was wrong and what it costs. */
            offset += (int)x86_instr_len(&mem[offset],
                              MEMORY_RANGE - ((x86_pc + offset) & MEMORY_MASK));
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
            emit_bl(g_helpers.dinstr_jcond_rel8);
            offset += 2;
            instr_count++;
            goto block_done;
        }

        /* ---- Opcodes 0x80-0x8F: ModR/M group and data-movement ---- */

        case 0x80: case 0x81: case 0x82: case 0x83:
            /* GRP1 Eb/Ib and Ev/Iv (ADD/OR/ADC/SBB/AND/SUB/XOR/CMP) — defer to interpreter */
            emit_mov_reg(0, 4);
            emit_bl(g_helpers.dinstr_exec_one);
            /* Charge the instruction its REAL byte length, not 1:
             * `offset` becomes block->x86_len, the block's invalidation
             * footprint and the input to expected_end_ip. See
             * x86_instr_len() for why 1 was wrong and what it costs. */
            offset += (int)x86_instr_len(&mem[offset],
                              MEMORY_RANGE - ((x86_pc + offset) & MEMORY_MASK));
            instr_count++;
            goto block_done;

        case 0x84:  /* TEST Gb, Eb */
            /* ModR/M-based — defer to interpreter */
            emit_mov_reg(0, 4);
            emit_bl(g_helpers.dinstr_exec_one);
            /* Charge the instruction its REAL byte length, not 1:
             * `offset` becomes block->x86_len, the block's invalidation
             * footprint and the input to expected_end_ip. See
             * x86_instr_len() for why 1 was wrong and what it costs. */
            offset += (int)x86_instr_len(&mem[offset],
                              MEMORY_RANGE - ((x86_pc + offset) & MEMORY_MASK));
            instr_count++;
            goto block_done;

        case 0x85:  /* TEST Gv, Ev */
            emit_mov_reg(0, 4);
            emit_bl(g_helpers.dinstr_exec_one);
            /* Charge the instruction its REAL byte length, not 1:
             * `offset` becomes block->x86_len, the block's invalidation
             * footprint and the input to expected_end_ip. See
             * x86_instr_len() for why 1 was wrong and what it costs. */
            offset += (int)x86_instr_len(&mem[offset],
                              MEMORY_RANGE - ((x86_pc + offset) & MEMORY_MASK));
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
            emit_bl(g_helpers.dinstr_exec_one);
            /* Charge the instruction its REAL byte length, not 1:
             * `offset` becomes block->x86_len, the block's invalidation
             * footprint and the input to expected_end_ip. See
             * x86_instr_len() for why 1 was wrong and what it costs. */
            offset += (int)x86_instr_len(&mem[offset],
                              MEMORY_RANGE - ((x86_pc + offset) & MEMORY_MASK));
            instr_count++;
            goto block_done;

        case 0x8C:  /* MOV Ew, Sw */
            emit_mov_reg(0, 4);
            emit_bl(g_helpers.dinstr_exec_one);
            /* Charge the instruction its REAL byte length, not 1:
             * `offset` becomes block->x86_len, the block's invalidation
             * footprint and the input to expected_end_ip. See
             * x86_instr_len() for why 1 was wrong and what it costs. */
            offset += (int)x86_instr_len(&mem[offset],
                              MEMORY_RANGE - ((x86_pc + offset) & MEMORY_MASK));
            instr_count++;
            goto block_done;

        case 0x8D:  /* LEA Gv, M */
            emit_mov_reg(0, 4);
            emit_bl(g_helpers.dinstr_exec_one);
            /* Charge the instruction its REAL byte length, not 1:
             * `offset` becomes block->x86_len, the block's invalidation
             * footprint and the input to expected_end_ip. See
             * x86_instr_len() for why 1 was wrong and what it costs. */
            offset += (int)x86_instr_len(&mem[offset],
                              MEMORY_RANGE - ((x86_pc + offset) & MEMORY_MASK));
            instr_count++;
            goto block_done;

        case 0x8E:  /* MOV Sw, Ew */
            emit_mov_reg(0, 4);
            emit_bl(g_helpers.dinstr_exec_one);
            /* Charge the instruction its REAL byte length, not 1:
             * `offset` becomes block->x86_len, the block's invalidation
             * footprint and the input to expected_end_ip. See
             * x86_instr_len() for why 1 was wrong and what it costs. */
            offset += (int)x86_instr_len(&mem[offset],
                              MEMORY_RANGE - ((x86_pc + offset) & MEMORY_MASK));
            instr_count++;
            goto block_done;

        case 0x8F:  /* POP Ev */
            emit_mov_reg(0, 4);
            emit_bl(g_helpers.dinstr_exec_one);
            /* Charge the instruction its REAL byte length, not 1:
             * `offset` becomes block->x86_len, the block's invalidation
             * footprint and the input to expected_end_ip. See
             * x86_instr_len() for why 1 was wrong and what it costs. */
            offset += (int)x86_instr_len(&mem[offset],
                              MEMORY_RANGE - ((x86_pc + offset) & MEMORY_MASK));
            instr_count++;
            goto block_done;

        default:
            /* Unsupported opcode — bail out to interpreter */
            goto translate_fail;
        }

        if (emit32_failed) {
            goto translate_fail;
        }

        /* This instruction ran to completion without touching cpu->ip (no
         * control flow, no interpreter fallback), so carry its byte length
         * forward and let the next iteration's flush commit it. */
        pending_ip += (uint32_t)offset - prev_offset;

        instr_count++;
    }

block_done:
    /* Check if code cache overflowed during translation */
    if (emit32_failed) {
        goto translate_fail;
    }

    /* Commit any IP advance still owed. Reached with pending_ip == 0 when
     * we got here via `goto block_done` (the terminator helper already set
     * cpu->ip itself); non-zero when the loop simply ran out of
     * instructions, in which case the last few need their bytes committed. */
    emit_advance_ip(pending_ip);
    pending_ip = 0;
    if (emit32_failed) {
        goto translate_fail;
    }

    /* Function epilogue */
    ARM_POP();  /* restore r4, r10, r11, pc */

/* Store in block cache, reusing a slot invalidated since the last flush */
    {
        block_entry_t* slot = alloc_block_slot();
        if (slot) {
            slot->x86_pc = x86_pc;
            slot->arm_offset = start_pos;
            slot->code_len = code_cache_pos - start_pos;
            slot->x86_len = offset;   /* BYTES of guest x86 code (true length) */
            slot->instr_count = (uint32_t)instr_count;  /* x86 INSTRUCTIONS */
            slot->x86_end_ip = (uint16_t)(cpu->ip + offset);
            slot->valid = 1;
         }
     }

    /* Flush the I-cache for this block's generated code. This is done ONCE,
     * at translation time, NOT on every execution (see the comment at the
     * execution site in dynrec_exec()). On ARMv5TE, cacheflush() is a
     * syscall; calling it per-block-execution was the dominant perf bug. */
    cacheflush((long)(code_cache_base + start_pos),
                (long)(code_cache_pos),
                0x3);

    return instr_count;

translate_fail:
    /* Restore code cache position to discard any partially emitted garbage */
    code_cache_pos = start_pos;
    return -1;
}

void dynrec_init(void) {
    /* Start with dynrec disabled - must be explicitly enabled */
    flush_all_blocks();
    dynrec_enabled = 0;
    DLOGI("dynrec: initialized (disabled by default - enable via setspeed or nativeRun)");
}

void dynrec_reset(void) {
    if (!dynrec_enabled) return;
    flush_all_blocks();
    DLOGI("dynrec: reset");
}

void dynrec_enable(void) {
    if (dynrec_enabled) return;

    /* Resolve all helper function addresses NOW — after the shared library
     * has been loaded at its runtime base. Under -fPIC, compile-time addresses
     * are link-time virtual addresses that do not match the relocates runtime
     * addresses; taking them here captures the correct values for emit_bl(). */
    dynrec_resolve_helpers();

    if (code_cache_base == NULL) {
        /* Map CODE_CACHE_SIZE + 4KB guard page. The guard page (PROT_NONE)
         * catches runaway execution that falls off the end of the code cache,
         * giving a clean SIGSEGV at the guard page instead of silent corruption. */
        void* base = mmap(NULL, CODE_CACHE_SIZE + 4096,
                          PROT_READ | PROT_WRITE | PROT_EXEC,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (base == MAP_FAILED) {
            DLOGI("dynrec: mmap failed on enable");
            code_cache_base = NULL;
            return;
        }
        /* Protect the guard page (last 4KB) as PROT_NONE */
        void* guard_page = (uint8_t*)base + CODE_CACHE_SIZE;
        if (mprotect(guard_page, 4096, PROT_NONE) != 0) {
            DLOGI("dynrec: mprotect guard page failed");
            /* Not fatal - continue without guard page */
        }
        code_cache_base = (uint8_t*)base;
    }
    flush_all_blocks();
    dynrec_enabled = 1;
    crashlog("ENABLE: code_cache_base=%p (guard at %p)", code_cache_base,
             (void*)((uint8_t*)code_cache_base + CODE_CACHE_SIZE));
    DLOGI("dynrec: enabled, code_cache_base=%p", code_cache_base);
}

void dynrec_disable(void) {
    dynrec_enabled = 0;
    DLOGI("dynrec: disabled");
}

void dynrec_invalidate_range(uint32_t start, uint32_t len) {
    if (!dynrec_enabled) return;
    uint32_t end = start + len;
    /*
	Interval intersection, not a start-address test. cpu_write() calls this
	with len==1 for every byte written to RAM, so testing only
	block.x86_pc == start meant a patch landing anywhere inside a
	translated block left the stale block in the cache to be executed
	again. A write at the very first byte of a block is rare; writes into
	the interior are what actually happen (code loads, patches, SMC).
    */
    for (int i = 0; i < block_cache_count; i++) {
        uint32_t block_end;
        if (!block_cache[i].valid) continue;
        /*
         * The footprint is [x86_pc, x86_pc + x86_len) and x86_len is the TRUE
         * byte length of the block's x86 code, so a write to any byte of any
         * instruction in the block -- including the ModR/M, SIB, displacement
         * and immediate bytes of an instruction the block handed to the
         * interpreter -- lands inside it. Charging those instructions 1 byte
         * each (the old behaviour) left their remaining bytes outside the
         * range, and a patch there failed to invalidate the block. See
         * x86_instr_len() for how the length is obtained and why it never
         * under-estimates.
         */
        block_end = block_cache[i].x86_pc + block_cache[i].x86_len;
        if (block_cache[i].x86_pc < end && block_end > start) {
            DLOGI("invalidate: block at 0x%05X len=%u (instrs=%u) overlaps "
                  "write 0x%05X..0x%05X",
                  block_cache[i].x86_pc, block_cache[i].x86_len,
                  block_cache[i].instr_count, start, end);
            block_cache[i].valid = 0;
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

    if (block != NULL) {
        /* Safety check: block must be within executable code cache region.
         * If somehow a block lands in the guard page, invalidate it and fall back. */
        if (block->arm_offset >= CODE_CACHE_SIZE) {
            crashlog("BLOCK_IN_GUARD: x86_pc=0x%05X arm_off=%u >= CODE_CACHE_SIZE, invalidating",
                     x86_pc, block->arm_offset);
            block->valid = 0;
            block = NULL;
        }
    }

    if (block != NULL) {
     } else {
        int result = translate_block(cpu, x86_pc, MAX_INSTRUCTIONS);
        if (result < 0 || result == 0) {
            /* Can't translate or empty block — use interpreter */
             int batch = (max_instr - total_executed);
             if (batch > 1000) batch = 1000;
             cpu_exec(cpu, batch);
            dynrec_interpreter_instrs += batch;
            total_executed += batch;
            continue;
        }
        /*
            Look the block back up rather than assuming it landed in the
            last slot: alloc_block_slot() reuses a freed slot wherever it
            finds one, so the entry can be anywhere in the array.
        */
        block = find_block(x86_pc);
         if (block == NULL) {
             /* Translation reported success but left nothing cached — treat
              * it as untranslatable rather than executing a garbage block. */
             cpu_exec(cpu, 1);
            dynrec_interpreter_instrs += 1;
            total_executed += 1;
            continue;
        }
    }

    /* Execute the translated block.
     *
     * NOTE: cacheflush() is NOT called here — it is unnecessary to flush on
     * every block execution. The block's code was flushed once at translation
     * time (in translate_block(), after the instructions were emitted). On
     * ARMv5TE, cacheflush() is a SWI→kernel syscall costing ~1-2µs. Calling
     * it per-block (instead of per-translation) was the dominant performance
     * bottleneck: with single-instruction blocks and 1000-instruction batches,
     * that meant ~1000 syscalls per batch, starving the 8088 core on the
     * 528MHz single-core IS01. */

    /* Execute the translated block */
    typedef void (*block_func_t)(CPU_t* cpu);
    block_func_t func = (block_func_t)(code_cache_base + block->arm_offset);

    uint16_t ip_before = cpu->ip;
    uint16_t expected_end_ip = (uint16_t)(cpu->ip + block->x86_len);

    /* Crash attribution globals only — no logging on the hot path. */
    g_cur_arm_off = block->arm_offset;
    g_cur_code_len = block->code_len;
    g_cur_x86_pc = x86_pc;
    g_cur_cpu = (uint32_t)(uintptr_t)cpu;
    /* NOT the block's R4 — the C caller's R4. Only used for the after-call
     * epilogue comparison below. */
    g_cur_r4_in = dynrec_read_r4();

    g_active_blocks++;
    func(cpu);
    g_active_blocks--;

    /* A caller_r4 != exit_r4 mismatch means the block's epilogue
     * `pop {r4,...}` failed to restore R4. */
    if (g_cur_r4_in != dynrec_read_r4()) {
        crashlog("R4_CORRUPT: x86_pc=0x%05X caller_r4=0x%08X exit_r4=0x%08X",
                 x86_pc, (unsigned)g_cur_r4_in, (unsigned)dynrec_read_r4());
    }

    g_cur_arm_off = 0xFFFFFFFFu;
    g_cur_code_len = 0;
    g_cur_x86_pc = 0;
    g_cur_cpu = 0;
    g_cur_r4_in = 0;

    dynrec_native_blocks++;

    /* The generated code advances cpu->ip itself (see emit_advance_ip), so
     * after a block that had no terminator cpu->ip should already equal
     * expected_end_ip. A block ending in jmp/call/ret/int legitimately
     * leaves it elsewhere, so only the unchanged case is suspicious. */
    if (cpu->ip == ip_before && block->x86_len > 0) {
        crashlog("IP_DESYNC: x86_pc=0x%05X ip unchanged at 0x%04X, expected 0x%04X",
                 x86_pc, ip_before, expected_end_ip);
        cpu->ip = expected_end_ip;
    }

    /* Removed duplicate redundant check — this was a copy-paste error */
    if (cpu->hltstate || cpu->trap_toggle) {
        total_executed += block->instr_count;
        continue;
    }

    /*
     * Count INSTRUCTIONS executed, not bytes. x86_len is a byte count and the
     * two only coincide for blocks of 1-byte instructions: a block holding one
     * 4-byte JZ would have charged 4, and a block holding a block of MOV AX
     * (3 bytes each) 15 for 5 instructions. dynrec_exec()'s return value is a
     * budget against max_instr, so it has to be counted in instructions.
     */
    total_executed += block->instr_count;
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

volatile int dynrec_enabled = 0;

void dynrec_init(void) {}
void dynrec_reset(void) {}
int dynrec_exec(CPU_t* cpu, I8259_t* i8259, int max_instr) {
    (void)cpu; (void)i8259; (void)max_instr;
    return 0;
}
void dynrec_invalidate_range(uint32_t start, uint32_t len) {
    (void)start; (void)len;
}
int dynrec_get_code_cache(uintptr_t *base, uint32_t *size) {
    if (base) *base = 0;
    if (size) *size = 0;
    return 0;
}
int dynrec_get_current_block(uint32_t *arm_off, uint32_t *code_len, uint32_t *x86_pc) {
    (void)arm_off; (void)code_len; (void)x86_pc;
    return 0;
}
int dynrec_get_current_entry(uint32_t *entry_cpu, uint32_t *caller_r4) {
    if (entry_cpu) *entry_cpu = 0;
    if (caller_r4) *caller_r4 = 0;
    return 0;
}

#endif
