#ifndef _DEBUGLOG_H_
#define _DEBUGLOG_H_

#include <stdint.h>

/*
	Debug levels:

	0 - No logging
	1 - Errors
	2 - Errors, info
	3 - Errors, info, detailed debugging
*/

#define DEBUG_NONE		0
#define DEBUG_ERROR		1
#define DEBUG_INFO		2
#define DEBUG_DETAIL	3

void debug_log(uint8_t level, char* format, ...);
void debug_setLevel(uint8_t level);
void debug_init();

/*
	Trace categories — independently toggleable at runtime via JNI.

	Previously a single `xt_trace_enabled` boolean gated every __android_log_print
	call. On the single-core ARMv5TE target each logd write is a blocking syscall
	(0.5-2 ms), so tracing everything from the IRQ/timing hot path consumed so much
	of the sole CPU core that the 8088 emulation crawled — key events went
	unprocessed and the UI thread starved.

	trace_flags defaults to 0 (everything off). Enable only the category you
	need, via nativeSetTraceFlag() from Java or the on-device debug menu.
*/

#define TRACE_FLAG_NONE         0x00u
#define TRACE_FLAG_CPU          0x01u  /* CPU execution, IP sampling, nativeRun loop */
#define TRACE_FLAG_KEYBOARD     0x02u  /* Key events, scancode injection, i8255 PPI */
#define TRACE_FLAG_DISK         0x04u  /* biosdisk, fdc */
#define TRACE_FLAG_TIMERS       0x08u  /* i8253 PIT, timing_loop */
#define TRACE_FLAG_INTERRUPTS   0x10u  /* i8259 PIC, IRQ raise/acknowledge */
#define TRACE_FLAG_VIDEO        0x20u  /* vga, framebuffer copy */
#define TRACE_FLAG_MISC         0x40u  /* machine init, utility, audio, JNI glue */
#define TRACE_FLAG_DIAG         0x80u  /* /sdcard file diagnostics (diag.h DIAG) */
#define TRACE_FLAG_ALL          0xFFu

extern volatile uint8_t trace_flags;

/* Runtime setters — safe to call from the UI thread via JNI. */
void debug_setTrace(uint8_t flags);
void debug_setTraceCategory(uint8_t category, uint8_t on);

#endif
