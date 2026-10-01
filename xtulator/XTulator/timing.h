#ifndef _TIMING_H_
#define _TIMING_H_

#include <stdint.h>

typedef struct TIMER_s {
	uint64_t interval;
	uint64_t previous;
	uint8_t enabled;
	/*
		Maximum number of times this timer's callback may fire within a single
		timing_loop() call while catching up on elapsed host time.

		timing_loop() used to fire every timer at most once per call, so a timer
		registered at 48kHz could only ever run at the main loop rate (~60Hz
		here). That made the PIT -- and therefore the emulated clock and the BIOS
		tick counter -- run ~800x slow, so every BIOS timing loop took forever
		and the machine looked hung. Timers now catch up, bounded by this value
		so a slow host cannot make a single call run away.
	*/
	uint32_t catchup;
	void (*callback)(void*);
	void* data;
} TIMER;

#define TIMING_ENABLED	1
#define TIMING_DISABLED	0
#define TIMING_ERROR 0xFFFFFFFF

/*
	Default catch-up budget. Generous, so cheap timers track wall-clock time
	faithfully. Callbacks that are genuinely expensive per call (see
	timing_setCatchup) must be given a smaller budget.
*/
#define TIMING_CATCHUP_DEFAULT	4096
#define TIMING_CATCHUP_MAX		0xFFFFFFFFu

#define TIMING_RINGSIZE	1024

int timing_init();
void timing_loop();
uint32_t timing_addTimer(void* callback, void* data, double frequency, uint8_t enabled);
void timing_updateIntervalFreq(uint32_t tnum, double frequency);
void timing_updateInterval(uint32_t tnum, uint64_t interval);
void timing_setCatchup(uint32_t tnum, uint32_t catchup);
void timing_speedTest();
void timing_timerEnable(uint32_t tnum);
void timing_timerDisable(uint32_t tnum);
uint64_t timing_getFreq();
uint64_t timing_getCur();
uint32_t timing_getDropped(uint32_t tnum);

extern uint64_t timing_cur;
extern uint64_t timing_freq;

#endif
