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

#include "config.h"
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include "debuglog.h"
#ifdef __ANDROID__
#include <pthread.h>
#endif

uint8_t debug_level = DEBUG_NONE;

/*
	Per-category trace flags. ALL default to off (0). Flip individual bits at
	runtime via debug_setTraceCategory() or the JNI nativeSetTraceFlag() method.
*/
volatile uint8_t trace_flags = 0;

/* Shared DIAG stream state; see diag.h for why this lives here and not in the header. */
FILE* diag_fp = NULL;
volatile int diag_initialized = 0;
volatile uint32_t diag_pending = 0;
#ifdef __ANDROID__
pthread_mutex_t diag_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

void debug_log(uint8_t level, char* format, ...) {
	va_list argptr;
	va_start(argptr, format);
	if (level > debug_level) {
		va_end(argptr);
		return;
	}
	vfprintf(stderr, format, argptr);
	fflush(stderr);
	va_end(argptr);
}

void debug_setLevel(uint8_t level) {
	if (level > DEBUG_DETAIL) {
		return;
	}
	debug_level = level;
}

void debug_init() {
	//TODO: Maybe allow initializing this with file output rather than always using stderr. Or maybe remove this, I don't know...
}

void debug_setTrace(uint8_t flags) {
	trace_flags = flags;
}

void debug_setTraceCategory(uint8_t category, uint8_t on) {
	if (on) {
		trace_flags |= category;
	} else {
		trace_flags &= ~category;
	}
}
