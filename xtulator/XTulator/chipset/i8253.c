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
	Intel 8253 timer
*/

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include "i8253.h"
#include "i8259.h"
#include "../config.h"
#include "../modules/audio/pcspeaker.h"
#include "../timing.h"
#include "../ports.h"
#include "../debuglog.h"
#include "../diag.h"
#ifdef __ANDROID__
#include <android/log.h>
#define I8253_LOGI(...) do { if (trace_flags & TRACE_FLAG_TIMERS) { __android_log_print(ANDROID_LOG_INFO, "XTulator-I8253", __VA_ARGS__); } } while (0)
#else
#define I8253_LOGI(...)
#endif

uint32_t i8253_timers[3];

/*
	Diagnostic: how often channel 0 actually raises IRQ0, and what the BIOS
	programmed it to. INT 8 (the BIOS timer tick at 0040:006C) is the single
	hottest routine in the whole emulator, so if this rate is far above the
	~18.2Hz the BIOS expects, the PIT model is the problem.
*/
uint64_t i8253_irq0_count = 0;
static I8253_t* i8253_diag_inst = NULL;

void i8253_debugReset(void) {
	i8253_irq0_count = 0;
}

void i8253_debugDump(uint64_t host_us) {
	I8253_t* i8253 = i8253_diag_inst;
	if (i8253 == NULL) {
		DIAG("PIT: no instance");
		return;
	}
	DIAG("PIT: IRQ0=%llu in %llums -> %.2f Hz (BIOS expects ~18.2Hz); ch0 reload=%u mode=%u counter=%u active=%u",
		(unsigned long long)i8253_irq0_count,
		(unsigned long long)(host_us / 1000),
		host_us ? ((double)i8253_irq0_count * 1000000.0 / (double)host_us) : 0.0,
		i8253->reload[0], i8253->mode[0], i8253->counter[0], i8253->active[0]);
	i8253_irq0_count = 0;
}

void i8253_write(I8253_t* i8253, uint16_t portnum, uint8_t value) {
	uint8_t sel, rl, loaded;
	portnum &= 3;

	loaded = 0;
	switch (portnum) {
	case 0: //load counters
	case 1:
	case 2:
		switch (i8253->rlmode[portnum]) {
		case 1: //MSB only
			i8253->reload[portnum] = (int32_t)value << 8;
			i8253->active[portnum] = 1;
			loaded = 1;
			break;
		case 2: //LSB only
			i8253->reload[portnum] = value;
			i8253->active[portnum] = 1;
			loaded = 1;
			break;
		case 3: //LSB, then MSB
			if (i8253->dataflipflop[portnum] == 0) { //LSB
				i8253->reload[portnum] = (i8253->reload[portnum] & 0xFF00) | value;
			} else { //MSB
				i8253->reload[portnum] = (i8253->reload[portnum] & 0x00FF) | ((int32_t)value << 8);
				i8253->counter[portnum] = i8253->reload[portnum];
				if (i8253->reload[portnum] == 0) {
					i8253->reload[portnum] = 65536;
				}
				i8253->active[portnum] = 1;
				loaded = 1;
#ifdef DEBUG_PIT
				debug_log(DEBUG_DETAIL, "I8253: Counter %u reload = %d\r\n", portnum, i8253->reload[portnum]);
#endif
			}
			i8253->dataflipflop[portnum] ^= 1;
			break;
		}
		if (loaded) switch (i8253->mode[portnum]) {
		case 0:
		case 1:
			i8253->out[portnum] = 0;
			break;
		case 2:
		case 3:
			i8253->out[portnum] = 1;
			break;
		}
		break;
	case 3: //control word
		sel = value >> 6;
		if (sel == 3) { //illegal
			return;
		}
		rl = (value >> 4) & 3; //read/load mode
		if (rl == 0) { //counter latching operation
			i8253->latch[sel] = i8253->counter[sel];
		} else { //set mode
			i8253->rlmode[sel] = rl;
			i8253->mode[sel] = (value >> 1) & 7;
			if (i8253->mode[sel] & 0x02) {
				i8253->mode[sel] &= 3; //MSB is "don't care" if bit 1 is set
			}
			i8253->bcd[sel] = value & 1;
#ifdef DEBUG_PIT
			debug_log(DEBUG_DETAIL, "I8253: Counter %u mode = %u\r\n", sel, i8253->mode[sel]);
#endif
		}
		i8253->dataflipflop[sel] = 0;
		break;
	}
}

uint8_t i8253_read(I8253_t* i8253, uint16_t portnum) {
	uint8_t ret;
	portnum &= 3;

	if (portnum == 3) {
		return 0xFF; //no read of control word possible
	}

	switch (i8253->rlmode[portnum]) {
	case 1: //MSB only
		return i8253->latch[portnum] >> 8;
	case 2: //LSB only
		return (uint8_t)i8253->latch[portnum];
	default: //LSB, then MSB (case 3, but say default so MSVC stops warning me about control paths not all returning a value)
		if (i8253->dataflipflop[portnum] == 0) { //LSB
			ret = (uint8_t)i8253->latch[portnum];
		} else { //MSB
			ret = i8253->latch[portnum] >> 8;
		}
		return ret;
	}
}

void i8253_timerCallback0(I8259_t* i8259) {
    I8253_LOGI("i8253_timerCallback0: raising IRQ0");
	i8253_irq0_count++;
	i8259_doirq(i8259, 0);
}

void i8253_timerCallback1(I8259_t* i8259) {
}

void i8253_timerCallback2(I8259_t* i8259) {
}

void i8253_tickCallback(I8253CB_t* i8253cb) {
    I8253_LOGI("i8253_tickCallback: called");
	I8253_t* i8253;
	I8259_t* i8259;
	uint8_t i;

	i8253 = i8253cb->i8253;
	i8259 = i8253cb->i8259;
	for (i = 0; i < 3; i++) {
		if ((i == 2) && (i8253->mode[2] != 3)) pcspeaker_setGateState(i8253->cbdata.pcspeaker, PC_SPEAKER_GATE_TIMER2, 0);
		if (i8253->active[i]) switch (i8253->mode[i]) {
		case 0: //interrupt on terminal count
			if (i8253->counter[i] > 0) {
				i8253->counter[i] = 0;
				i8253->out[i] = 1;
				if (i == 0) i8253_timerCallback0(i8259);
			}
			break;
		case 2: //rate generator
			i8253->out[i] ^= 1;
			if (i == 0) i8253_timerCallback0(i8259);
			i8253->counter[i] = i8253->reload[i];
			break;
		case 3: //square wave generator
			i8253->out[i] ^= 1;
			if (i8253->out[i] == 0) {
				if (i == 0) i8253_timerCallback0(i8259);
			}
			if (i == 2) pcspeaker_setGateState(i8253->cbdata.pcspeaker, PC_SPEAKER_GATE_TIMER2, i8253->out[i]);
			i8253->counter[i] = i8253->reload[i];
			break;
		default:
#ifdef DEBUG_PIT
			debug_log(DEBUG_DETAIL, "I8253: Unknown mode %u on counter %u\r\n", i8253->mode[i], i);
#endif
			break;
		}
	}
}

void i8253_init(I8253_t* i8253, I8259_t* i8259, PCSPEAKER_t* pcspeaker) {
	memset(i8253, 0, sizeof(I8253_t));

	i8253_diag_inst = i8253;

	i8253->cbdata.i8253 = i8253;
	i8253->cbdata.i8259 = i8259;
	i8253->cbdata.pcspeaker = pcspeaker;

	i8253->timer_id = timing_addTimer(i8253_tickCallback, (void*)(&i8253->cbdata), 1000000.0 / 18.2, TIMING_ENABLED); // 18.2 Hz timer tick
	timing_setCatchup(i8253->timer_id, 5);

	ports_cbRegister(0x40, 4, (void*)i8253_read, NULL, (void*)i8253_write, NULL, i8253);
}
