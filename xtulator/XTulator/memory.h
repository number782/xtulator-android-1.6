#ifndef _MEMORY_H_
#define _MEMORY_H_

#include <stdint.h>

#define MEMORY_RANGE		0x100000
#define MEMORY_MASK			0x0FFFFF

/* Exposed for dynrec block translation — allows checking if an address
   is direct-mapped RAM (not MMIO) before translating a block. */
extern uint8_t* memory_mapRead[];
extern uint8_t* memory_mapWrite[];
extern uint8_t (*memory_mapReadCallback[])(void* udata, uint32_t addr);
extern void (*memory_mapWriteCallback[])(void* udata, uint32_t addr, uint8_t value);

void memory_mapRegister(uint32_t start, uint32_t len, uint8_t* readb, uint8_t* writeb);
void memory_mapCallbackRegister(uint32_t start, uint32_t count, uint8_t(*readb)(void*, uint32_t), void (*writeb)(void*, uint32_t, uint8_t), void* udata);
int memory_init();

#endif
