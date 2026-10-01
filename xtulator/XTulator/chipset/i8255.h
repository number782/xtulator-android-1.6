#ifndef _I8255_H_
#define _I8255_H_

#include <stdint.h>
#include "../modules/audio/pcspeaker.h"
#include "../modules/input/input.h"

typedef struct {
	uint8_t sw2;
	uint8_t portA;
	uint8_t portB;
	uint8_t portC;
	KEYSTATE_t* keystate;
	PCSPEAKER_t* pcspeaker;
	// Keyboard controller (8042) emulation
	uint8_t kbc_status;       // Port 0x64 status register
	uint8_t kbc_data;         // Port 0x60 data register (output buffer)
	uint8_t kbc_command;      // Last command written to port 0x64
	uint8_t kbc_wait_response; // Waiting for response after command
	uint8_t kbc_response;     // Response byte for KBC commands
	uint8_t kbc_has_response; // Flag indicating response is ready
	uint8_t kbc_expect_data;  // Flag expecting data byte after command
} I8255_t;

uint8_t i8255_readport(I8255_t* i8255, uint16_t portnum);
void i8255_writeport(I8255_t* i8255, uint16_t portnum, uint8_t value);
void i8255_init(I8255_t* i8255, KEYSTATE_t* keystate, PCSPEAKER_t* pcspeaker);

#endif
