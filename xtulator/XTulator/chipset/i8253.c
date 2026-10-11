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
	Intel 8253 timer -- lazy model.

	The upstream core ticks every channel from a 48 kHz timer. That is ~48k
	callbacks per emulated second, which a 528 MHz ARM9 cannot afford.  The port
	then replaced it with a timer whose "frequency" argument was really a period
	(1000000/18.2 = 54945 Hz), which made IRQ0 fire at ~27 kHz and ignored what
	the BIOS programmed into the counters.

	This version never ticks.  A counter's value is *computed on demand* from the
	host time elapsed since it was loaded (the PIT input clock is 1.193182 MHz),
	so reads are exact and cost nothing when nobody reads them.  The only timer is
	one that raises IRQ0 at the rate channel 0 was programmed for.  The IRQ0 rate
	follows the BIOS/game programming (18.2 Hz by default) and is clamped to
	PIT_IRQ_MAX_HZ, because the guest cannot service interrupts faster than that
	on this hardware anyway (and the 8259 IRR is a latch, so extra edges would be
	merged).

	Time base: timing_cur, the clock sample timing_loop() already took.  It is at
	most one batch of instructions stale, which avoids a clock_gettime() syscall
	per port read.
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

#define PIT_CLOCK_HZ		1193182ULL
#define PIT_IRQ_MAX_HZ		2000.0

uint32_t i8253_timers[3];

static I8253_t* i8253_diag_inst = NULL;
uint64_t i8253_irq0_count = 0;

void i8253_debugReset(void) {
	i8253_irq0_count = 0;
}

void i8253_debugDump(uint64_t host_us) {
	I8253_t* i8253 = i8253_diag_inst;
	if (i8253 == NULL) {
		DIAG("PIT: no instance");
		return;
	}
	DIAG("PIT: IRQ0=%llu in %llums -> %.2f Hz (BIOS expects ~18.2Hz); ch0 reload=%d mode=%u active=%u",
		(unsigned long long)i8253_irq0_count,
		(unsigned long long)(host_us / 1000),
		host_us ? ((double)i8253_irq0_count * 1000000.0 / (double)host_us) : 0.0,
		(int)i8253->reload[0], i8253->mode[0], i8253->active[0]);
	i8253_irq0_count = 0;
}

/* Number of PIT input clocks since the counter was loaded. */
static uint64_t i8253_elapsed(I8253_t* i8253, uint8_t ch) {
	uint64_t dt = timing_cur - i8253->loadtime[ch];
	if (timing_cur < i8253->loadtime[ch]) return 0;
	return (dt * PIT_CLOCK_HZ) / 1000000ULL;
}

static uint32_t i8253_reloadval(I8253_t* i8253, uint8_t ch) {
	uint32_t r = (uint32_t)i8253->reload[ch];
	return (r == 0) ? 65536 : r;
}

/* Current counter value, as the hardware would show it. */
static uint16_t i8253_count(I8253_t* i8253, uint8_t ch) {
	uint64_t t;
	uint32_t r, v;

	if (!i8253->active[ch]) return (uint16_t)i8253->counter[ch];
	t = i8253_elapsed(i8253, ch);
	r = i8253_reloadval(i8253, ch);
	switch (i8253->mode[ch]) {
	case 2: //rate generator: r..1 repeating
		v = r - (uint32_t)(t % r);
		break;
	case 3: //square wave: counts down by two, so a full period is r input clocks
		v = r - (uint32_t)((2 * (t % r)) % r);
		break;
	default: //mode 0/1/4/5: counts down once, then wraps
		v = (uint32_t)((r - (t & 0xFFFF)) & 0xFFFF);
		break;
	}
	return (uint16_t)v;
}

/* Output pin of a channel, derived from time. */
static uint8_t i8253_outpin(I8253_t* i8253, uint8_t ch) {
	uint64_t t;
	uint32_t r;

	if (!i8253->active[ch]) return i8253->out[ch];
	t = i8253_elapsed(i8253, ch);
	r = i8253_reloadval(i8253, ch);
	switch (i8253->mode[ch]) {
	case 0:
		return (t >= r) ? 1 : 0;
	case 2:
		return ((t % r) == (r - 1)) ? 0 : 1;
	case 3:
		return ((t % r) < ((r + 1) / 2)) ? 1 : 0;
	}
	return i8253->out[ch];
}

uint8_t i8253_getOut2(void) {
	if (i8253_diag_inst == NULL) return 0;
	return i8253_outpin(i8253_diag_inst, 2);
}

static void i8253_irq0_apply(I8253_t* i8253) {
	/* (Re)program the IRQ0 timer from channel 0's current mode and reload. */
	double hz;
	uint32_t r;

	if (!i8253->active[0] || (i8253->mode[0] != 2 && i8253->mode[0] != 3 && i8253->mode[0] != 0)) {
		timing_timerDisable(i8253->timer_id);
		return;
	}
	r = i8253_reloadval(i8253, 0);
	hz = (double)PIT_CLOCK_HZ / (double)r;
	if (hz > PIT_IRQ_MAX_HZ) hz = PIT_IRQ_MAX_HZ;
	timing_updateIntervalFreq(i8253->timer_id, hz);
	timing_timerEnable(i8253->timer_id); //also restarts the interval from "now"
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
	I8253_t* i8253 = (I8253_t*)i8253cb->i8253;
	i8253_timerCallback0(i8253cb->i8259);
	if (i8253->mode[0] == 0) { //interrupt on terminal count: one shot
		i8253->out[0] = 1;
		timing_timerDisable(i8253->timer_id);
	}
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
			loaded = 1;
			break;
		case 2: //LSB only
			i8253->reload[portnum] = value;
			loaded = 1;
			break;
		case 3: //LSB, then MSB
			if (i8253->wff[portnum] == 0) { //LSB
				i8253->reload[portnum] = (i8253->reload[portnum] & 0xFF00) | value;
			} else { //MSB
				i8253->reload[portnum] = (i8253->reload[portnum] & 0x00FF) | ((int32_t)value << 8);
				loaded = 1;
			}
			i8253->wff[portnum] ^= 1;
			break;
		}
		if (loaded) {
			if (i8253->reload[portnum] == 0) {
				i8253->reload[portnum] = 65536;
			}
			i8253->counter[portnum] = i8253->reload[portnum];
			i8253->active[portnum] = 1;
			i8253->loadtime[portnum] = timing_cur;
			switch (i8253->mode[portnum]) {
			case 0:
			case 1:
				i8253->out[portnum] = 0;
				break;
			case 2:
			case 3:
				i8253->out[portnum] = 1;
				break;
			}
#ifdef DEBUG_PIT
			debug_log(DEBUG_DETAIL, "I8253: Counter %u reload = %d\r\n", portnum, i8253->reload[portnum]);
#endif
			if (portnum == 0) i8253_irq0_apply(i8253);
		}
		break;
	case 3: //control word
		sel = value >> 6;
		if (sel == 3) { //illegal (8254 read-back, not on the 8253)
			return;
		}
		rl = (value >> 4) & 3; //read/load mode
		if (rl == 0) { //counter latching operation
			if (!i8253->latched[sel]) {
				i8253->latch[sel] = i8253_count(i8253, sel);
				i8253->latched[sel] = 1;
				i8253->rff[sel] = 0;
			}
		} else { //set mode
			i8253->rlmode[sel] = rl;
			i8253->mode[sel] = (value >> 1) & 7;
			if (i8253->mode[sel] & 0x02) {
				i8253->mode[sel] &= 3; //MSB is "don't care" if bit 1 is set
			}
			i8253->bcd[sel] = value & 1;
			i8253->wff[sel] = 0;
			i8253->rff[sel] = 0;
			i8253->latched[sel] = 0;
			i8253->active[sel] = 0; //a new mode waits for a new count to be loaded
			if (sel == 0) timing_timerDisable(i8253->timer_id);
#ifdef DEBUG_PIT
			debug_log(DEBUG_DETAIL, "I8253: Counter %u mode = %u\r\n", sel, i8253->mode[sel]);
#endif
		}
		break;
	}
}

uint8_t i8253_read(I8253_t* i8253, uint16_t portnum) {
	uint8_t ret;
	portnum &= 3;

	if (portnum == 3) {
		return 0xFF; //no read of control word possible
	}

	/* No latch pending: sample the live count (what the hardware does when
	   software reads a counter without latching it first). */
	if (!i8253->latched[portnum]) {
		i8253->latch[portnum] = i8253_count(i8253, portnum);
	}

	switch (i8253->rlmode[portnum]) {
	case 1: //MSB only
		ret = i8253->latch[portnum] >> 8;
		i8253->latched[portnum] = 0;
		return ret;
	case 2: //LSB only
		ret = (uint8_t)i8253->latch[portnum];
		i8253->latched[portnum] = 0;
		return ret;
	default: //LSB, then MSB
		if (i8253->rff[portnum] == 0) { //LSB
			ret = (uint8_t)i8253->latch[portnum];
			i8253->rff[portnum] = 1;
			i8253->latched[portnum] = 1; //hold the value so the MSB belongs to the same sample
		} else { //MSB
			ret = i8253->latch[portnum] >> 8;
			i8253->rff[portnum] = 0;
			i8253->latched[portnum] = 0;
		}
		return ret;
	}
}

void i8253_init(I8253_t* i8253, I8259_t* i8259, PCSPEAKER_t* pcspeaker) {
	memset(i8253, 0, sizeof(I8253_t));

	i8253_diag_inst = i8253;

	i8253->cbdata.i8253 = i8253;
	i8253->cbdata.i8259 = i8259;
	i8253->cbdata.pcspeaker = pcspeaker;

	/* The only PIT timer: raises IRQ0.  Disabled until channel 0 is programmed,
	   and retargeted to the programmed rate by i8253_irq0_apply(). */
	i8253->timer_id = timing_addTimer(i8253_tickCallback, (void*)(&i8253->cbdata), 18.2, TIMING_DISABLED);
	timing_setCatchup(i8253->timer_id, 2);

	ports_cbRegister(0x40, 4, (void*)i8253_read, NULL, (void*)i8253_write, NULL, i8253);
}
