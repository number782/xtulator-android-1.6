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


#ifdef _WIN32
#include <Windows.h>
#else
#include <time.h>
#endif
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "config.h"
#include "timing.h"
#include "debuglog.h"

#ifdef __ANDROID__
#include <android/log.h>
#define TIMING_LOGI(...) do { if (trace_flags & TRACE_FLAG_TIMERS) { __android_log_print(ANDROID_LOG_INFO, "XTulator-TIMING", __VA_ARGS__); } } while (0)
#else
#define TIMING_LOGI(...)
#endif

uint64_t timing_cur;
uint64_t timing_freq;
TIMER* timers = NULL;
uint32_t timers_count = 0;
/* Per-timer count of catch-up budgets that were exhausted; see timing_loop(). */
static uint32_t* timers_dropped = NULL;
#define TIMING_REFRESH_EVERY 64u
static uint32_t timing_refresh_counter = 0;
int timing_init() {
	TIMING_LOGI("timing_init called");
	debug_log(DEBUG_INFO, "[TIMING] timing_init called\r\n");
#ifdef _WIN32
	LARGE_INTEGER freq;
	//TODO: error handling
	QueryPerformanceFrequency(&freq);
	timing_freq = (uint64_t)freq.QuadPart;
#else
	timing_freq = 1000000;
#endif
	return 0;
}

void timing_loop() {
	uint32_t i;
#ifdef _WIN32
	LARGE_INTEGER cur;
	//TODO: error handling
	QueryPerformanceCounter(&cur);
	timing_cur = (uint64_t)cur.QuadPart;
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	timing_cur = (uint64_t)ts.tv_sec * 1000000 + (uint64_t)(ts.tv_nsec / 1000);
#endif
	for (i = 0; i < timers_count; i++) {
		if (timing_cur < (timers[i].previous + timers[i].interval)) {
			continue; //not due yet
		}

		if (timers[i].enabled == TIMING_DISABLED) {
			/*
				Resync a disabled timer, otherwise it stays permanently "due" and
				the first thing that happens after it is re-enabled is a huge
				bogus catch-up burst.
			*/
			timers[i].previous = timing_cur;
			continue;
		}

		/*
			Catch up on elapsed host time, bounded by timers[i].catchup. Without
			this the effective rate of every timer was the main loop rate rather
			than its registered frequency.
		*/
		{
			uint32_t fired = 0;
			uint32_t budget = timers[i].catchup;
			if (budget == 0) {
				budget = 1;
			}
			while (timing_cur >= (timers[i].previous + timers[i].interval)) {
				if (timers[i].callback != NULL) {
					(*timers[i].callback)(timers[i].data);
				}
				timers[i].previous += timers[i].interval;
				if (++fired >= budget) {
					break;
				}
			}
			if (timing_cur >= (timers[i].previous + timers[i].interval)) {
				/*
					Still behind after the budget: the host cannot sustain this
					rate. Skip the backlog and resync, otherwise every subsequent
					call would try to re-catch-up and fall further behind.
				*/
				if (timers_dropped != NULL) {
					timers_dropped[i]++;
				}
				timers[i].previous = timing_cur - timers[i].interval;
			}
		}
	}
}

//Just some code for performance testing
void timing_speedTest() {
#ifdef _WIN32
	uint64_t start, i;
	LARGE_INTEGER cur;
	//TODO: error handling
	QueryPerformanceCounter(&cur);
	start = (uint64_t)cur.QuadPart;

	i = 0;
	while (1) {
		QueryPerformanceCounter(&cur);
		timing_cur = (uint64_t)cur.QuadPart;
		i++;
		if ((timing_cur - start) >= timing_freq) break;
	}
	printf("%llu calls to QPC in 1 second\r\n", i);
#endif
}



uint32_t timing_addTimerUsingInterval(void* callback, void* data, uint64_t interval, uint8_t enabled) {
	TIMER* temp;
	uint32_t ret;
#ifdef _WIN32
	LARGE_INTEGER cur;

	//TODO: error handling
	QueryPerformanceCounter(&cur);
	timing_cur = (uint64_t)cur.QuadPart;
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	timing_cur = (uint64_t)ts.tv_sec * 1000000 + (uint64_t)(ts.tv_nsec / 1000);
#endif
	TIMING_LOGI("Adding timer %u, interval=%llu, callback=%p", timers_count, (unsigned long long)interval, callback);
	temp = (TIMER*)realloc(timers, (size_t)sizeof(TIMER) * (timers_count + 1));
	TIMING_LOGI("realloc result=%p, timers_count=%u", temp, timers_count);
	if (temp == NULL) {
		//TODO: error handling
		TIMING_LOGI("realloc FAILED!");
		return TIMING_ERROR; //NULL;
	}
	timers = temp;

	/* Keep the dropped-event counters the same size as the timer array. */
	{
		uint32_t* d = (uint32_t*)realloc(timers_dropped, (size_t)sizeof(uint32_t) * (timers_count + 1));
		if (d != NULL) {
			timers_dropped = d;
			timers_dropped[timers_count] = 0;
		}
	}

	timers[timers_count].previous = timing_cur;
	timers[timers_count].interval = interval;
	timers[timers_count].catchup = TIMING_CATCHUP_DEFAULT;
	timers[timers_count].callback = callback;
	timers[timers_count].data = data;
	timers[timers_count].enabled = enabled;

	ret = timers_count;
	timers_count++;
	TIMING_LOGI("Timer added successfully, new count=%u", timers_count);

	return ret;
}

uint32_t timing_addTimer(void* callback, void* data, double frequency, uint8_t enabled) {
	return timing_addTimerUsingInterval(callback, data, (uint64_t)((double)timing_freq / frequency), enabled);
}

void timing_updateInterval(uint32_t tnum, uint64_t interval) {
	if (tnum >= timers_count) {
		debug_log(DEBUG_ERROR, "[ERROR] timing_updateInterval() asked to operate on invalid timer\r\n");
		return;
	}
	timers[tnum].interval = interval;
}

void timing_updateIntervalFreq(uint32_t tnum, double frequency) {
	if (tnum >= timers_count) {
		debug_log(DEBUG_ERROR, "[ERROR] timing_updateIntervalFreq() asked to operate on invalid timer\r\n");
		return;
	}
	timers[tnum].interval = (uint64_t)((double)timing_freq / frequency);
}

void timing_setCatchup(uint32_t tnum, uint32_t catchup) {
	if (tnum >= timers_count) {
		debug_log(DEBUG_ERROR, "[ERROR] timing_setCatchup() asked to operate on invalid timer\r\n");
		return;
	}
	timers[tnum].catchup = (catchup == 0) ? 1 : catchup;
}

uint32_t timing_getDropped(uint32_t tnum) {
	if (tnum >= timers_count || timers_dropped == NULL) {
		return 0;
	}
	return timers_dropped[tnum];
}

void timing_timerEnable(uint32_t tnum) {
	if (tnum >= timers_count) {
		debug_log(DEBUG_ERROR, "[ERROR] timing_timerEnable() asked to operate on invalid timer\r\n");
		return;
	}
	timers[tnum].enabled = TIMING_ENABLED;
	/*
		Reuse the sample timing_loop() already took. timing_getCur() would cost
		another clock_gettime(CLOCK_MONOTONIC), measured at 0.25-1.3ms on the
		being one loop iteration stale makes no practical difference.
	*/
	timers[tnum].previous = timing_cur;
}

void timing_timerDisable(uint32_t tnum) {
	if (tnum >= timers_count) {
		debug_log(DEBUG_ERROR, "[ERROR] timing_timerDisable() asked to operate on invalid timer\r\n");
		return;
	}
	timers[tnum].enabled = TIMING_DISABLED;
}

uint64_t timing_getFreq() {
	return timing_freq;
}

uint64_t timing_getCur() {
#ifdef _WIN32
	LARGE_INTEGER cur;

	//TODO: error handling
	QueryPerformanceCounter(&cur);
	timing_cur = (uint64_t)cur.QuadPart;
#else
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	timing_cur = (uint64_t)ts.tv_sec * 1000000 + (uint64_t)(ts.tv_nsec / 1000);
#endif

	return timing_cur;
}
