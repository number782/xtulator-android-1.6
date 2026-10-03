#ifndef _DYNREC_H_
#define _DYNREC_H_

#include <stdint.h>
#include "cpu.h"
#include "../chipset/i8259.h"

void dynrec_init(void);
void dynrec_reset(void);
int dynrec_exec(CPU_t* cpu, I8259_t* i8259, int max_instr);
void dynrec_invalidate_range(uint32_t start, uint32_t len);
void dynrec_enable(void);  /* enable dynrec at runtime */
void dynrec_disable(void);

extern volatile int dynrec_enabled;

/* Dynrec statistics */
extern volatile uint32_t dynrec_native_blocks;
extern volatile uint64_t dynrec_interpreter_instrs;
uint32_t dynrec_get_native_block_count(void);
uint64_t dynrec_get_interpreter_instr_count(void);

/* Crash attribution — lets the native crash handler tell a fault inside a
 * generated block apart from a fault in a helper, and dump the offending
 * ARM instructions plus the x86 PC that produced them. */
int dynrec_get_code_cache(uintptr_t *base, uint32_t *size);
int dynrec_get_current_block(uint32_t *arm_off, uint32_t *code_len, uint32_t *x86_pc);
/* Same block, plus:
 *   *entry_cpu — the cpu pointer that was passed to func(cpu), sampled from C
 *                immediately before the call and cleared to 0 immediately
 *                after it returns. 0 means no dynrec block is active.
 *                IMPORTANT: do NOT compare entry_cpu against fault-time R4.
 *                R4 is a callee-saved register that EVERY C function — including
 *                each dinstr_* helper and cpu_exec — is free to reuse for its own
 *                locals. The block's prologue `mov r4, r0` sets the BLOCK's R4,
 *                but once a helper is on the stack, fault-time R4 is the helper's
 *                own value (very often the guest x86_pc), NOT the block's. The
 *                cpu pointer is the ARGUMENT carried in R0 at each helper-entry
 *                `mov r0, r4`; check fault-time R0 (== entry_cpu) instead.
 *   *caller_r4 — R4 of dynrec_exec's OWN C frame, sampled just before
 *                func(cpu). Not comparable to fault-time R4; kept only for the
 *                after-call epilogue check.
 *   *guest_bytes_out — copies up to `count` byte(s) of the guest instruction
 *                bytes starting at *x86_pc (the guest *physical* address,
 *                (cs<<4)+ip mod 1MB). Handy for the crash handler to report
 *                which x86 opcode the active block was translating/executing
 *                when a helper faulted. Returns the number of bytes written
 *                (0 if the address is unmapped/MIO). */
int dynrec_get_current_entry(uint32_t *entry_cpu, uint32_t *caller_r4);
int dynrec_get_guest_bytes(uint32_t x86_pc, uint32_t count, uint8_t *out);

#endif
