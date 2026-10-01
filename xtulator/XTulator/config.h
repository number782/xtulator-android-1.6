#ifndef _CONFIG_H_
#define _CONFIG_H_

#include <stdint.h>

#define STR_TITLE "XTulator"
#define STR_VERSION "0.24.5.24"

//#define DEBUG_DMA
//#define DEBUG_VGA
//#define DEBUG_CGA
//#define DEBUG_PIT
//#define DEBUG_PIC
//#define DEBUG_PPI
//#define DEBUG_UART
//#define DEBUG_TCPMODEM
//#define DEBUG_PCSPEAKER
//#define DEBUG_MEMORY
//#define DEBUG_PORTS
//#define DEBUG_TIMING
//#define DEBUG_OPL2
//#define DEBUG_BLASTER
//#define DEBUG_FDC
//#define DEBUG_NE2000
//#define DEBUG_PCAP

#define USE_DISK_HLE
#define USE_NUKED_OPL
#ifndef __ANDROID__
#define USE_NE2000
#endif

#ifdef _WIN32
#define ENABLE_TCP_MODEM
#endif

#define VIDEO_CARD_MDA		0
#define VIDEO_CARD_CGA		1
#define VIDEO_CARD_EGA		2
#define VIDEO_CARD_VGA		3

#define SAMPLE_RATE		48000
#define SAMPLE_BUFFER	4800

#ifdef _WIN32
#define FUNC_INLINE __forceinline
#else
#define FUNC_INLINE inline
#endif

#ifndef _WIN32
#define _stricmp strcasecmp
#endif

/*
	Per-category trace flags — see debuglog.h for the full list. The single
	master `xt_trace_enabled` boolean was replaced by a bitmask (trace_flags)
	so you can toggle CPU / keyboard / disk logging independently at runtime
	via the in-app debug menu or the JNI nativeSetTraceFlag() call. All flags
	default to 0 (off) so the hot path never pays the logd cost unless you
	explicitly ask for it.

	Audio output: set to 0 to skip generating samples entirely.

	Audio is by far the most expensive per-timer-callback in the emulator: one
	sdlaudio_generateSample() runs a whole Nuked OPL3 sample. At SAMPLE_RATE
	(48kHz) that is ~48k core iterations per emulated second, which on a
	528MHz single-core target competes directly with interpreting the 8088.
	Set to 1 to turn audio back on.
*/
#define XT_ENABLE_AUDIO 0

extern volatile uint8_t running;
extern uint8_t videocard, showMIPS;
extern double speedarg;
extern volatile double speed;
extern uint32_t baudrate, ramsize;
extern char* usemachine;
extern uint8_t bootdrive;
extern volatile uint32_t cpu_speed_pct;

void setspeed(double mhz);

#endif
