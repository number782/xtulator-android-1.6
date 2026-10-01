#ifndef _DIAG_H_
#define _DIAG_H_

#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

#include "debuglog.h"

#ifdef __ANDROID__
#define DIAG_FILE "/sdcard/xtulator/diag.log"
#else
#define DIAG_FILE "diag.log"
#endif

/*
	DIAG writes go to /sdcard, which is FUSE-backed on the target: a single
	write()+fflush() there costs 8-10ms. Flushing on every call made the
	diagnostics more expensive than the emulation they were measuring, so the
	stream is left block-buffered and only flushed every DIAG_FLUSH_EVERY calls.
	Also note the state below used to be `static` in this header, which gave every
	translation unit its own FILE* and its own mutex all appending to the same
	file; it now lives in debuglog.c and is shared.
*/
#define DIAG_FLUSH_EVERY 64

#ifdef __ANDROID__
#include <pthread.h>
extern pthread_mutex_t diag_mutex;
#endif
extern FILE* diag_fp;
extern volatile int diag_initialized;
extern volatile uint32_t diag_pending;
extern volatile uint8_t trace_flags;

static void diag_init(void) {
	if (diag_initialized) return;
	diag_fp = fopen(DIAG_FILE, "a");
	if (diag_fp) {
		fprintf(diag_fp, "\n=== XTulator diagnostic log ===\n");
		fflush(diag_fp);
	}
	diag_initialized = 1;
}

static void diag_log(const char* fmt, ...) {
	va_list ap;
	if (!diag_initialized) diag_init();
	if (!diag_fp) return;
#ifdef __ANDROID__
	pthread_mutex_lock(&diag_mutex);
#endif
	va_start(ap, fmt);
	vfprintf(diag_fp, fmt, ap);
	va_end(ap);
	fprintf(diag_fp, "\n");
	/* Block-buffered: let stdio coalesce writes instead of paying 8-10ms a line. */
	if (++diag_pending >= DIAG_FLUSH_EVERY) {
		diag_pending = 0;
		fflush(diag_fp);
	}
#ifdef __ANDROID__
	pthread_mutex_unlock(&diag_mutex);
#endif
}

#define DIAG(...) do { if (trace_flags & TRACE_FLAG_MISC) diag_log(__VA_ARGS__); } while (0)
#endif
