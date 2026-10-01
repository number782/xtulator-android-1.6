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

#endif
