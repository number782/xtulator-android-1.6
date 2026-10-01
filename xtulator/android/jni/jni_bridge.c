#include <jni.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <android/log.h>

#include "android_frontend.h"

#include "../XTulator/config.h"
#include "../XTulator/ports.h"
#include "../XTulator/memory.h"
#include "../XTulator/machine.h"
#include "../XTulator/cpu/cpu.h"
#include "../XTulator/cpu/dynrec.h"
#include "../XTulator/chipset/i8259.h"
#include "../XTulator/chipset/i8253.h"
#include "../XTulator/modules/disk/biosdisk.h"
#include "../XTulator/modules/video/vga.h"
#include "../XTulator/modules/audio/sdlaudio.h"
#include "../XTulator/modules/input/mouse.h"
#include "../XTulator/timing.h"
#include "../XTulator/debuglog.h"
#include "../XTulator/diag.h"
#include "android_frontend.h"

#define LOG_TAG "XTulator-JNI"
#define CPU_SPEED_REF_MHZ 4.77
/*
	LOGI/LOGE/LOGW are gated on TRACE_FLAG_MISC so they only fire when
	the "other" tracing category is explicitly enabled.  Keyboard-specific
	logging uses LOGKEY (TRACE_FLAG_KEYBOARD) and per-iteration CPU sampling
	uses LOGCPU (TRACE_FLAG_CPU), keeping the hot path logd-free by default.
*/
#define LOGI(...) do { if (trace_flags & TRACE_FLAG_MISC) { __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__); } } while (0)
#define LOGE(...) do { if (trace_flags & TRACE_FLAG_MISC) { __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__); } } while (0)
#define LOGW(...) do { if (trace_flags & TRACE_FLAG_MISC) { __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__); } } while (0)
#define LOGKEY(...) do { if (trace_flags & TRACE_FLAG_KEYBOARD) { __android_log_print(ANDROID_LOG_INFO, "XTulator-KEY", __VA_ARGS__); } } while (0)
#define LOGCPU(...) do { if (trace_flags & TRACE_FLAG_CPU) { __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__); } } while (0)

volatile uint8_t running = 1;
volatile uint32_t cpu_speed_pct = 0;
uint8_t videocard = 0xFF, showMIPS = 0;
volatile double speed = 0;
uint32_t baudrate = 115200, ramsize = 640;
char* usemachine = "generic_xt";

MACHINE_t machine;

static uint32_t optimer_ticks = 0;
static int profile_watch = -1;
static uint64_t pit_last_dump = 0;
/* When unthrottled (speed=0), run larger batches to reduce per-iteration
 * overhead (2× cpu_interruptCheck + timing_loop per batch). 1000 instructions
 * per batch gives ~10× better throughput than 100 while keeping key latency
 * well under the BIOS typematic rate. */
#define UNTHROTTLED_BATCH 1000
static uint32_t instructionsperloop = UNTHROTTLED_BATCH, cpuLimitTimer;
static volatile uint8_t goCPU = 1, limitCPU = 0, emu_paused = 0;
/* Ring buffer for key scancodes (press=0x00-0x7F, release=0x80-0xFF).
 * Size must be power of 2 for fast modulo. */
#define KEYBUF_SIZE 16
static uint8_t keybuf[KEYBUF_SIZE];
static volatile uint32_t keybuf_head = 0, keybuf_tail = 0;
static char title[64];

static void optimer(void* dummy) {
	/*
		optimer runs at 10Hz. We now measure actual retired instructions
		(cpu->totalexec) instead of the naive ops counter, because ops
		included instruction slots skipped during HALT — inflating the
		speed reading to 147-300% when the CPU is idle at A:\>.
	*/
	(void)dummy;
	CPU_t* cpu = &machine.CPU;
	static uint64_t last_totalexec = 0;
	uint64_t retired = cpu->totalexec - last_totalexec;
	last_totalexec = cpu->totalexec;
	{
		double mips = ((double)retired * 10.0) / 1000000.0;
		cpu_speed_pct = (uint32_t)(mips / CPU_SPEED_REF_MHZ * 100.0 + 0.5);
		if (showMIPS) {
			debug_log(DEBUG_INFO, "%.3f MIPS (%u%%)\r", mips, (unsigned)cpu_speed_pct);
		}
	}
	if ((trace_flags & TRACE_FLAG_MISC) && ++optimer_ticks >= 10) {
		optimer_ticks = 0;
		DIAG("PERF: %.3f MIPS (%llu instructions in the last 0.1s)",
			((double)retired * 10.0) / 1000000.0, (unsigned long long)retired);
		/* Read the hottest address before cpu_profileDump() resets the table. */
		profile_watch = (int)cpu_hottest();
		cpu_profileDump();
		/* Dump the code around whatever the sampler just found hottest. */
		if (profile_watch > 0 && profile_watch != 0xFFFFFFFF) {
			cpu_dumpmem(cpu, (uint32_t)profile_watch, 48);
		}
		i8253_debugDump(timing_getCur() - pit_last_dump);
		pit_last_dump = timing_getCur();
		i8253_debugReset();
	}
}

static void cputimer(void* dummy) {
	(void)dummy;
	goCPU = 1;
}

void setspeed(double mhz) {
	if (mhz > 0) {
		speed = mhz;
		instructionsperloop = (uint32_t)(speed * 1e6 / 140000.0);
		limitCPU = 1;
		timing_timerEnable(cpuLimitTimer);
	} else {
		speed = 0;
		instructionsperloop = UNTHROTTLED_BATCH;
		limitCPU = 0;
		timing_timerDisable(cpuLimitTimer);
	}
}

JNIEXPORT jint JNICALL Java_com_xtulator_android_XTulatorActivity_nativeInit(JNIEnv *env, jobject thiz, jstring biosPath, jstring diskPath) {
	const char* biosPathStr;
	const char* lastSlash;
	size_t dlen;
	char dirbuf[1024];
	const char* diskPathStr;

 	(void)thiz;

 	LOGE("nativeInit: CALLED");
 	/* Reset run state — nativeStop may have set running=0 if the Activity
 	 * was destroyed but the process survived. Without this, the emulator
 	 * thread in onResume exits immediately and the screen stays black. */
 	running = 1;
 	emu_paused = 0;
 	goCPU = 1;
	sprintf(title, "%s v%s", STR_TITLE, STR_VERSION);
	LOGI("nativeInit: title=%s", title);
	DIAG("nativeInit: starting");

	if (biosPath != NULL) {
		biosPathStr = (*env)->GetStringUTFChars(env, biosPath, NULL);
		if (biosPathStr != NULL) {
			lastSlash = strrchr(biosPathStr, '/');
			if (lastSlash != NULL) {
				dlen = (size_t)(lastSlash - biosPathStr);
				if (dlen < sizeof(dirbuf)) {
					memcpy(dirbuf, biosPathStr, dlen);
					dirbuf[dlen] = '\0';
					LOGI("nativeInit: chdir to %s", dirbuf);
					int chdir_ret = chdir(dirbuf);
					LOGI("nativeInit: chdir return value = %d", chdir_ret);
					if (chdir_ret != 0) {
						LOGI("nativeInit: chdir failed: %s", strerror(errno));
						DIAG("nativeInit: chdir to '%s' FAILED: %s", dirbuf, strerror(errno));
					} else {
						DIAG("nativeInit: chdir to '%s' OK", dirbuf);
					}

				}
			}
			(*env)->ReleaseStringUTFChars(env, biosPath, biosPathStr);
		}
	}

	LOGI("nativeInit: calling ports_init");
	ports_init();
	LOGI("nativeInit: calling timing_init");
	timing_init();
	LOGI("nativeInit: calling memory_init");
	memory_init();
	machine.pcap_if = -1;

	if (diskPath != NULL) {
		diskPathStr = (*env)->GetStringUTFChars(env, diskPath, NULL);
		if (diskPathStr != NULL) {
			LOGI("nativeInit: calling biosdisk_insert with %s", diskPathStr);
			char diskPathCopy[1024];
			strncpy(diskPathCopy, diskPathStr, sizeof(diskPathCopy) - 1);
			diskPathCopy[sizeof(diskPathCopy) - 1] = '\0';
			(*env)->ReleaseStringUTFChars(env, diskPath, diskPathStr);
			diskPathStr = NULL;
			biosdisk_insert(&machine.CPU, 0, diskPathCopy);
		}
	}

	LOGI("nativeInit: calling sdlconsole_init");
	if (sdlconsole_init(title) != 0) {
		LOGI("nativeInit: sdlconsole_init failed");
		return -1;
	}
	LOGI("nativeInit: sdlconsole_init OK");
	if (sdlaudio_init(&machine) != 0) {
		LOGI("nativeInit: sdlaudio_init failed");
	}
	LOGI("nativeInit: calling machine_init");
	if (machine_init(&machine, usemachine) < 0) {
		LOGI("nativeInit: machine_init failed");
		return -1;
	}
	LOGI("nativeInit: machine_init OK");

    /* Initialize dynamic recompiler */
    dynrec_init();
    /* Dynrec disabled by default — enable via menu toggle */
    if (dynrec_enabled) {
        LOGI("nativeInit: dynrec initialized successfully");
    } else {
        LOGI("nativeInit: dynrec not available, using interpreter");
    }

	if (bootdrive == 0xFF) {
		bootdrive = biosdisk[0].inserted ? 0x00 : (biosdisk[2].inserted ? 0x80 : 0x00);
	}

	LOGI("nativeInit: adding timers");
	timing_addTimer((void*)optimer, NULL, 10.0, TIMING_ENABLED);
	cpuLimitTimer = timing_addTimer((void*)cputimer, NULL, 10000.0, TIMING_DISABLED);
	if (speed > 0) {
		setspeed(speed);
	}

	DIAG("nativeInit: completed successfully");
	/*
		This function is declared jint and the success path used to fall off the
		end, so it returned whatever happened to be in the return register. The
		Java side treats any non-zero as "Native init failed" and logs it, which
		made a successful init look like a failure (observed: 1107444095).
	*/
	return 0;
}

JNIEXPORT void JNICALL Java_com_xtulator_android_XTulatorActivity_nativeRun(	JNIEnv *env, jobject thiz) {
	(void)env; (void)thiz;
	LOGE("nativeRun: starting (dynrec=%d, ipl=%u)", dynrec_enabled, instructionsperloop);

	uint32_t loop_count = 0;
	uint32_t diag_count = 0;  /* Count how many blocks were executed natively */
	while (running) {
		loop_count++;
		if (loop_count % 200 == 0) {
			LOGE("nativeRun: loop %u, IP=%04X:%04X, dynrec_exec=%u, native=%u",
				loop_count, machine.CPU.segregs[regcs], machine.CPU.ip,
				(unsigned)instructionsperloop, diag_count);
		}
		cpu_interruptCheck(&machine.CPU, &machine.i8259);

        /* Use dynrec if available, otherwise fall back to interpreter */
        if (dynrec_enabled) {
            int executed = dynrec_exec(&machine.CPU, &machine.i8259, instructionsperloop);
            if (executed == 0) {
                /* dynrec fell back entirely to interpreter for this batch */
                cpu_exec(&machine.CPU, instructionsperloop);
            } else {
                diag_count++;  /* Count native block executions */
            }
        } else {
            cpu_exec(&machine.CPU, instructionsperloop);
        }

		/* After cpu_exec, the interrupt handler may have run, sent EOI,
		 * and executed IRET (restoring IF). Check again before timing_loop
		 * fires the timer — this gives lower-priority IRQs (keyboard IRQ 1)
		 * a chance to be serviced instead of being perpetually starved by
		 * the re-raised timer IRQ 0. */
		cpu_interruptCheck(&machine.CPU, &machine.i8259);

		/* Pause check — must use goCPU alone, not (limitCPU && !goCPU),
		 * because when speed=0 (limitCPU=0) the old condition never slept,
		 * so the emulator ran at full tilt even when the Activity was paused. */
		if (!goCPU) {
			usleep(500);
		}

		timing_loop();

	  /* Drain one scancode per iteration: the 8088 reads port 0x60 once per
	   * IRQ1, so feeding a whole burst into KeyState at once would overwrite
	   * the pending byte and drop every key except the last. */
		if (keybuf_head != keybuf_tail) {
			uint8_t sc = keybuf[keybuf_tail];
			keybuf_tail = (keybuf_tail + 1) & (KEYBUF_SIZE - 1);
			LOGKEY("Injecting scancode=0x%02X", sc);
			machine.KeyState.scancode = sc;
			machine.KeyState.isNew = 1;
			i8259_doirq(&machine.i8259, 1);
		}
	}
	LOGE("nativeRun: emulation stopped");
}

JNIEXPORT void JNICALL Java_com_xtulator_android_XTulatorActivity_nativeStop(JNIEnv *env, jobject thiz) {
	(void)env; (void)thiz;
	LOGI("nativeStop: stopping emulation");
	DIAG("nativeStop: stopping emulation");
	running = 0;
}

JNIEXPORT void JNICALL Java_com_xtulator_android_XTulatorActivity_nativeKeyDown(JNIEnv *env, jobject thiz, jint keyCode) {
	(void)env; (void)thiz;
	LOGKEY("nativeKeyDown: keyCode=%d", (int)keyCode);
	uint8_t sc = android_keycode_to_scancode((int)keyCode);
	LOGKEY("nativeKeyDown: translated scancode=0x%02X", sc);
	if (sc != 0) {
		uint32_t next_head = (keybuf_head + 1) & (KEYBUF_SIZE - 1);
		if (next_head != keybuf_tail) {
			keybuf[keybuf_head] = sc;
			keybuf_head = next_head;
		} else {
			LOGKEY("nativeKeyDown: key buffer full, dropping scancode=0x%02X", sc);
		}
	}
}
JNIEXPORT void JNICALL Java_com_xtulator_android_XTulatorActivity_nativeKeyUp(JNIEnv *env, jobject thiz, jint keyCode) {
	(void)env; (void)thiz;
	LOGKEY("nativeKeyUp: keyCode=%d", (int)keyCode);
	uint8_t sc = android_keycode_to_scancode((int)keyCode);
	LOGKEY("nativeKeyUp: translated scancode=0x%02X (release=0x%02X)", sc, sc | 0x80);
	if (sc != 0) {
		uint32_t next_head = (keybuf_head + 1) & (KEYBUF_SIZE - 1);
		if (next_head != keybuf_tail) {
			keybuf[keybuf_head] = sc | 0x80;
			keybuf_head = next_head;
		} else {
			LOGKEY("nativeKeyUp: key buffer full, dropping release=0x%02X", sc | 0x80);
		}
	}
}

JNIEXPORT void JNICALL Java_com_xtulator_android_XTulatorActivity_nativeMouseEvent(JNIEnv *env, jobject thiz, jint action, jint x, jint y) {
	(void)env; (void)thiz;
	LOGI("nativeMouseEvent: action=%d, x=%d, y=%d", (int)action, (int)x, (int)y);
	mouse_action((uint8_t)action, MOUSE_PRESSED, (int32_t)x, (int32_t)y);
}

JNIEXPORT void JNICALL Java_com_xtulator_android_XTulatorActivity_nativeResize(JNIEnv *env, jobject thiz, jint width, jint height) {
	(void)env; (void)thiz;
	LOGI("nativeResize: width=%d, height=%d", (int)width, (int)height);
	sdlconsole_setWindow((int)width, (int)height);
}

JNIEXPORT void JNICALL Java_com_xtulator_android_XTulatorActivity_nativePause(JNIEnv *env, jobject thiz) {
	(void)env; (void)thiz;
	LOGI("nativePause: pausing emulation");
	emu_paused = 1;
	goCPU = 0;
}

JNIEXPORT void JNICALL Java_com_xtulator_android_XTulatorActivity_nativeResume(JNIEnv *env, jobject thiz) {
	(void)env; (void)thiz;
	LOGI("nativeResume: resuming emulation");
	running = 1;
	emu_paused = 0;
	goCPU = 1;
}

JNIEXPORT jint JNICALL Java_com_xtulator_android_XTulatorActivity_nativeGetFbWidth(JNIEnv *env, jobject thiz) {
	(void)env; (void)thiz;
	int w, h;
	android_fb_get_dims(&w, &h);
	return w;
}

JNIEXPORT jint JNICALL Java_com_xtulator_android_XTulatorActivity_nativeGetFbHeight(JNIEnv *env, jobject thiz) {
	(void)env; (void)thiz;
	int w, h;
	android_fb_get_dims(&w, &h);
	return h;
}

JNIEXPORT jint JNICALL Java_com_xtulator_android_XTulatorActivity_nativeGetSpeedPct(JNIEnv *env, jobject thiz) {
	(void)env; (void)thiz;
	return (jint)cpu_speed_pct;
}

JNIEXPORT jint JNICALL Java_com_xtulator_android_XTulatorActivity_nativeGetDynrecNativeBlocks(JNIEnv *env, jobject thiz) {
	(void)env; (void)thiz;
	return (jint)dynrec_get_native_block_count();
}

JNIEXPORT jint JNICALL Java_com_xtulator_android_XTulatorActivity_nativeGetDynrecInterpreterInstrs(JNIEnv *env, jobject thiz) {
	(void)env; (void)thiz;
#if defined(__ANDROID__) && defined(__ARM_ARCH_5TE__)
	return (jint)(dynrec_get_interpreter_instr_count() & 0x7FFFFFFF);
#else
	return 0;
#endif
}

JNIEXPORT jint JNICALL Java_com_xtulator_android_XTulatorActivity_nativeCopyFrame(JNIEnv *env, jobject thiz, jintArray out) {
	(void)thiz;
	jint* buf = (*env)->GetIntArrayElements(env, out, NULL);
	if (buf == NULL) {
		return 0;
	}
	jsize len = (*env)->GetArrayLength(env, out);
	jint result = (jint)android_fb_copy((int32_t*)buf, (int)len);
	(*env)->ReleaseIntArrayElements(env, out, buf, 0);
	return result;
}

JNIEXPORT void JNICALL Java_com_xtulator_android_XTulatorActivity_nativeGetFbDims(JNIEnv *env, jobject thiz, jintArray dims) {
	(void)env; (void)thiz;
	jsize len = (*env)->GetArrayLength(env, dims);
	if (len < 2) {
		return;
	}
	int w, h;
	android_fb_get_dims(&w, &h);
	jint *arr = (*env)->GetIntArrayElements(env, dims, NULL);
	if (arr != NULL) {
		arr[0] = (jint)w;
		arr[1] = (jint)h;
		(*env)->ReleaseIntArrayElements(env, dims, arr, 0);
	}
}

JNIEXPORT jint JNICALL Java_com_xtulator_android_XTulatorActivity_nativeGetFrame(JNIEnv *env, jobject thiz, jintArray dims, jintArray pixels) {
	(void)thiz;
	jsize dimsLen = (*env)->GetArrayLength(env, dims);
	if (dimsLen < 2) {
		return 0;
	}
	jint *pixBuf = (*env)->GetIntArrayElements(env, pixels, NULL);
	if (pixBuf == NULL) {
		return 0;
	}
	jsize pixLen = (*env)->GetArrayLength(env, pixels);
	int w, h;
	jint result = (jint)android_fb_get_dims_and_copy(&w, &h, (int32_t*)pixBuf, (int)pixLen);
	(*env)->ReleaseIntArrayElements(env, pixels, pixBuf, 0);
	jint *dimsArr = (*env)->GetIntArrayElements(env, dims, NULL);
	if (dimsArr != NULL) {
		dimsArr[0] = (jint)w;
		dimsArr[1] = (jint)h;
		(*env)->ReleaseIntArrayElements(env, dims, dimsArr, 0);
	}
	return result;
}

JNIEXPORT void JNICALL Java_com_xtulator_android_XTulatorActivity_nativeReset(JNIEnv *env, jobject thiz) {
	(void)env; (void)thiz;
	LOGI("nativeReset: resetting emulator");
	cpu_reset(&machine.CPU);
	dynrec_reset();
	i8259_init(&machine.i8259);
	i8253_init(&machine.i8253, &machine.i8259, &machine.pcspeaker);
	i8237_init(&machine.i8237, &machine.CPU);
	i8255_init(&machine.i8255, &machine.KeyState, &machine.pcspeaker);
	uart_init(&machine.UART[0], &machine.i8259, 0x3F8, 4, NULL, NULL, NULL, NULL);
	uart_init(&machine.UART[1], &machine.i8259, 0x2F8, 3, NULL, NULL, NULL, NULL);
	pcspeaker_init(&machine.pcspeaker);
	blaster_init(&machine.blaster, &machine.i8237, &machine.i8259, 0x220, 1, 5);
	opl2_init(&machine.OPL2);
	fdc_init(&machine.fdc, &machine.CPU, &machine.i8259, &machine.i8237);
	biosdisk_init(&machine.CPU);
	// Re-insert current floppy if present
	if (biosdisk[0].inserted) {
		// Disk already open, just reset geometry
		biosdisk[0].cyls = 80;
		biosdisk[0].heads = 2;
		biosdisk[0].sects = 18;
	}
	bootdrive = biosdisk[0].inserted ? 0x00 : 0xFF;
	machine.KeyState.scancode = 0;
	machine.KeyState.isNew = 0;
	keybuf_head = keybuf_tail = 0;
	machine.CPU.totalexec = 0;
	goCPU = 1;
	LOGI("nativeReset: emulator reset complete");
}

JNIEXPORT jboolean JNICALL Java_com_xtulator_android_XTulatorActivity_nativeChangeFloppy(JNIEnv *env, jobject thiz, jstring path) {
	(void)thiz;
	const char* pathStr = (*env)->GetStringUTFChars(env, path, NULL);
	if (!pathStr) return JNI_FALSE;
	
	LOGI("nativeChangeFloppy: ejecting old, inserting %s", pathStr);
	
	// Eject current floppy (drive 0)
	if (biosdisk[0].inserted) {
		biosdisk_eject(&machine.CPU, 0);
	}
	
	// Insert new floppy
	char pathCopy[1024];
	strncpy(pathCopy, pathStr, sizeof(pathCopy) - 1);
	pathCopy[sizeof(pathCopy) - 1] = '\0';
	(*env)->ReleaseStringUTFChars(env, path, pathStr);
	
	if (biosdisk_insert(&machine.CPU, 0, pathCopy) == 0) {
		bootdrive = 0x00;
		LOGI("nativeChangeFloppy: success");
		return JNI_TRUE;
	} else {
		LOGI("nativeChangeFloppy: failed to insert");
		return JNI_FALSE;
	}
}

JNIEXPORT jboolean JNICALL Java_com_xtulator_android_XTulatorActivity_nativeCopyAsset(JNIEnv *env, jobject thiz, jobject assetManager, jstring assetName, jstring outPath) {
    (void)thiz;
    const char *assetNameStr = (*env)->GetStringUTFChars(env, assetName, NULL);
    const char *outPathStr = (*env)->GetStringUTFChars(env, outPath, NULL);
    if (!assetNameStr || !outPathStr) {
        if (assetNameStr) (*env)->ReleaseStringUTFChars(env, assetName, assetNameStr);
        if (outPathStr) (*env)->ReleaseStringUTFChars(env, outPath, outPathStr);
        return JNI_FALSE;
    }

    int result = android_copy_asset(env, assetManager, assetNameStr, outPathStr);

    (*env)->ReleaseStringUTFChars(env, assetName, assetNameStr);
    (*env)->ReleaseStringUTFChars(env, outPath, outPathStr);

    return (result >= 0) ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL Java_com_xtulator_android_XTulatorActivity_nativeSetTraceFlag(JNIEnv *env, jobject thiz, jint flag, jboolean enabled) {
	(void)env; (void)thiz;
	LOGI("nativeSetTraceFlag: flag=0x%02X enabled=%d", (unsigned)flag, (int)enabled);
	debug_setTraceCategory((uint8_t)flag, enabled ? 1 : 0);
}

JNIEXPORT void JNICALL Java_com_xtulator_android_XTulatorActivity_nativeEnableDynrec(JNIEnv *env, jobject thiz, jboolean enable) {
	(void)env; (void)thiz;
	if (enable) {
		dynrec_enable();
	} else {
		dynrec_disable();
	}
	LOGI("nativeEnableDynrec: enabled=%d (dynrec_enabled=%d)", (int)enable, dynrec_enabled);
}
