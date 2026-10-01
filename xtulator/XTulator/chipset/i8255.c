/*
  XTulator: A portable, open-source 80186 PC emulator.
  Copyright (C)2020 Mike Chambers

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License
  as published by the Free Software Foundation; either version 2
  of the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

/*
	Intel 8255 Programmable Peripheral Interface (PPI)

	This is not complete.
*/

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "../config.h"
#include "../timing.h"
#include "../modules/audio/pcspeaker.h"
#include "i8255.h"
#include "../ports.h"
#include "../debuglog.h"

#ifdef __ANDROID__
#include <android/log.h>
#define LOGKEY(...) do { if (trace_flags & TRACE_FLAG_KEYBOARD) { __android_log_print(ANDROID_LOG_INFO, "XTulator-KEY", __VA_ARGS__); } } while (0)
#else
#define LOGKEY(...)
#endif

static void kbc_process_command(I8255_t* i8255, uint8_t cmd) {
#ifdef DEBUG_PPI
	debug_log(DEBUG_INFO, "[KBC] Command: %02X\r\n", cmd);
#endif
	switch (cmd) {
	case 0xAA:
		i8255->kbc_response = 0x55;
		i8255->kbc_has_response = 1;
		i8255->kbc_status |= 0x01;
#ifdef DEBUG_PPI
		debug_log(DEBUG_INFO, "[KBC] Self-test passed, response=55\r\n");
#endif
		break;
	case 0xAB:
		i8255->kbc_response = 0x00;
		i8255->kbc_has_response = 1;
		i8255->kbc_status |= 0x01;
#ifdef DEBUG_PPI
		debug_log(DEBUG_INFO, "[KBC] Interface test\r\n");
#endif
		break;
	case 0xAD:
		i8255->kbc_status |= 0x10;
#ifdef DEBUG_PPI
		debug_log(DEBUG_INFO, "[KBC] Disable keyboard interface\r\n");
#endif
		break;
	case 0xAE:
		i8255->kbc_status &= ~0x10;
#ifdef DEBUG_PPI
		debug_log(DEBUG_INFO, "[KBC] Enable keyboard interface\r\n");
#endif
		break;
	case 0xC0:
		i8255->kbc_response = i8255->kbc_status;
		i8255->kbc_has_response = 1;
		i8255->kbc_status |= 0x01;
#ifdef DEBUG_PPI
		debug_log(DEBUG_INFO, "[KBC] Read input port\r\n");
#endif
		break;
	case 0xD0:
		i8255->kbc_response = i8255->kbc_status & 0xF0;
		i8255->kbc_has_response = 1;
		i8255->kbc_status |= 0x01;
#ifdef DEBUG_PPI
		debug_log(DEBUG_INFO, "[KBC] Read output port\r\n");
#endif
		break;
	case 0xE0:
		i8255->kbc_expect_data = 1;
#ifdef DEBUG_PPI
		debug_log(DEBUG_INFO, "[KBC] Read test inputs\r\n");
#endif
		break;
	case 0xF0:
	case 0xF1:
	case 0xF2:
	case 0xF3:
	case 0xF4:
	case 0xF5:
	case 0xF6:
	case 0xF7:
	case 0xF8:
	case 0xF9:
	case 0xFA:
	case 0xFB:
	case 0xFC:
	case 0xFD:
	case 0xFE:
	case 0xFF:
		i8255->kbc_expect_data = 1;
#ifdef DEBUG_PPI
		debug_log(DEBUG_INFO, "[KBC] Pulse output bit %u\r\n", cmd & 0x0F);
#endif
		break;
	default:
#ifdef DEBUG_PPI
		debug_log(DEBUG_INFO, "[KBC] Unknown command: %02X\r\n", cmd);
#endif
		break;
	}
}

uint8_t i8255_readport(I8255_t* i8255, uint16_t portnum) {
#ifdef DEBUG_PPI
	debug_log(DEBUG_INFO, "[I8255] Read port %02X\r\n", portnum);
#endif
	portnum &= 7;
	switch (portnum) {
	case 0:
		LOGKEY("i8255_readport: port 0x60 read, kbc_has_response=%d isNew=%d", i8255->kbc_has_response, i8255->keystate->isNew);
		if (i8255->kbc_has_response) {
			uint8_t response = i8255->kbc_response;
			i8255->kbc_has_response = 0;
			i8255->kbc_status &= ~0x01;
#ifdef DEBUG_PPI
			debug_log(DEBUG_INFO, "[KBC] Port 60 read response: %02X\r\n", response);
#endif
			LOGKEY("i8255_readport: returning KBC response 0x%02X", response);
			return response;
		}
		if (i8255->keystate->isNew) {
			uint8_t sc = i8255->keystate->scancode;
			i8255->keystate->isNew = 0;
#ifdef DEBUG_PPI
			debug_log(DEBUG_INFO, "[I8255] Port 60 read: scancode=%02X\r\n", sc);
#endif
			LOGKEY("i8255_readport: returning scancode 0x%02X", sc);
			return sc;
		}
		LOGKEY("i8255_readport: port 0x60 read, no data, returning 0xFF");
		return 0xFF;
	case 1:
		return i8255->portB;
	case 2:
		if (i8255->portB & 8) {
			return i8255->sw2 >> 4;
		} else {
			return i8255->sw2 & 0x0F;
		}
	case 4:
		return (i8255->kbc_has_response || i8255->keystate->isNew) ? 0x01 : 0x00;
	case 5:
		return i8255->kbc_status;
	}
	return 0xFF;
}

void i8255_writeport(I8255_t* i8255, uint16_t portnum, uint8_t value) {
#ifdef DEBUG_PPI
	debug_log(DEBUG_DETAIL, "[I8255] Write port %02X <- %02X\r\n", portnum, value);
#endif
	portnum &= 7;
	switch (portnum) {
	case 0:
		if (i8255->kbc_expect_data) {
#ifdef DEBUG_PPI
			debug_log(DEBUG_INFO, "[KBC] Data for command: %02X\r\n", value);
#endif
			i8255->kbc_expect_data = 0;
		} else {
			i8255->keystate->scancode = value;
#ifdef DEBUG_PPI
			debug_log(DEBUG_INFO, "[KBC] Port 60 write (to keyboard): %02X\r\n", value);
#endif
		}
		break;
	case 1:
		if (value & 0x01) {
			pcspeaker_selectGate(i8255->pcspeaker, PC_SPEAKER_USE_TIMER2);
#ifdef DEBUG_PPI
			debug_log(DEBUG_DETAIL, "[I8255] Speaker take input from timer 2\r\n");
#endif
		} else {
			pcspeaker_selectGate(i8255->pcspeaker, PC_SPEAKER_USE_DIRECT);
#ifdef DEBUG_PPI
			debug_log(DEBUG_DETAIL, "[I8255] Speaker take input from direct\r\n");
#endif
		}
		pcspeaker_setGateState(i8255->pcspeaker, PC_SPEAKER_GATE_DIRECT, (value >> 1) & 1);
#ifdef DEBUG_PPI
		debug_log(DEBUG_DETAIL, "[I8255] Speaker direct value = %u\r\n", (value >> 1) & 1);
#endif
		if ((value & 0x40) && !(i8255->portB & 0x40)) {
			i8255->keystate->scancode = 0xAA;
#ifdef DEBUG_PPI
			debug_log(DEBUG_DETAIL, "[I8255] Keyboard reset\r\n");
#endif
		}
		i8255->portB = (value & 0xEF) | (i8255->portB & 0x10);
		break;
	case 4:
		kbc_process_command(i8255, value);
		break;
	}
}

void i8255_refreshToggle(I8255_t* i8255) {
	i8255->portB ^= 0x10;
}

void i8255_init(I8255_t* i8255, KEYSTATE_t* keystate, PCSPEAKER_t* pcspeaker) {
	memset(i8255, 0, sizeof(I8255_t));
	i8255->keystate = keystate;
	i8255->pcspeaker = pcspeaker;
	i8255->kbc_status = 0x10;

	if (videocard == VIDEO_CARD_VGA) {
		i8255->sw2 = 0x46;
	}
	else if (videocard == VIDEO_CARD_CGA) {
		i8255->sw2 = 0x66;
	}

	ports_cbRegister(0x60, 6, (void*)i8255_readport, NULL, (void*)i8255_writeport, NULL, i8255);
	timing_addTimer(i8255_refreshToggle, i8255, 66667, TIMING_ENABLED);
}