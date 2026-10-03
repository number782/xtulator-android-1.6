#include <jni.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <unistd.h>
#include <signal.h>
#include <sys/ucontext.h>
#include <android/log.h>

#include "android_frontend.h"

/* Dynrec crash attribution. Declared here rather than via dynrec.h so this
 * file does not have to pull in the CPU / chipset headers. */
int dynrec_get_code_cache(uintptr_t *base, uint32_t *size);
int dynrec_get_current_block(uint32_t *arm_off, uint32_t *code_len, uint32_t *x86_pc);
int dynrec_get_current_entry(uint32_t *entry_cpu, uint32_t *caller_r4);

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

/* Map a fault PC through /proc/self/maps and log the containing mapping.
 *
 * Deliberately not dladdr(): the NDK r5c headers predate Dl_info (so it
 * does not even compile), and linking libdl purely for a crash path is not
 * worth the extra dependency. The mapping line carries the module path and
 * segment offset, which is enough to resolve the symbol against the .so
 * afterwards.
 */
static void dump_maps_line_for_pc(int fd, unsigned pc) {
    FILE *mf;
    char line[512];
    char out[640];
    int mfd;

    mfd = open("/proc/self/maps", O_RDONLY);
    if (mfd < 0) {
        int n = snprintf(out, sizeof(out), "  (cannot open /proc/self/maps)\n");
        write(fd, out, n);
        return;
    }
    /* fgets() needs a FILE*, not a raw descriptor. */
    mf = fdopen(mfd, "r");
    if (mf == NULL) {
        close(mfd);
        return;
    }
    while (fgets(line, sizeof(line), mf)) {
        unsigned long lo = 0, hi = 0;
        if (sscanf(line, "%lx-%lx", &lo, &hi) == 2 && pc >= lo && pc < hi) {
            int n;
            line[strcspn(line, "\n")] = '\0';
            n = snprintf(out, sizeof(out),
                         "  maps: %s   (offset in mapping = 0x%lx)\n",
                         line, (unsigned long)(pc - lo));
            write(fd, out, n);
            break;
        }
    }
    fclose(mf);
}

static void crash_signal_handler(int sig, siginfo_t *info, void *ctx) {
    int fd = open("/sdcard/xtulator_crash_detail.log",
                  O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd >= 0) {
        char buf[1024];
        int len;

        len = snprintf(buf, sizeof(buf),
            "CRASH: signal=%d errno=%d code=%d fault_addr=%p\n",
            sig, info ? info->si_errno : 0, info ? info->si_code : 0,
            info ? info->si_addr : (void *)0);
        write(fd, buf, len);

        if (ctx) {
            ucontext_t *uc = (ucontext_t *)ctx;
            len = snprintf(buf, sizeof(buf),
                "ARM REGISTERS:\n"
                "  R0=%08x  R1=%08x  R2=%08x  R3=%08x\n"
                "  R4=%08x  R5=%08x  R6=%08x  R7=%08x\n"
                "  R8=%08x  R9=%08x  R10=%08x R11=%08x\n"
                "  R12=%08x SP=%08x LR=%08x PC=%08x\n"
                "  CPSR=%08x\n",
                (unsigned)uc->uc_mcontext.arm_r0,
                (unsigned)uc->uc_mcontext.arm_r1,
                (unsigned)uc->uc_mcontext.arm_r2,
                (unsigned)uc->uc_mcontext.arm_r3,
                (unsigned)uc->uc_mcontext.arm_r4,
                (unsigned)uc->uc_mcontext.arm_r5,
                (unsigned)uc->uc_mcontext.arm_r6,
                (unsigned)uc->uc_mcontext.arm_r7,
                (unsigned)uc->uc_mcontext.arm_r8,
                (unsigned)uc->uc_mcontext.arm_r9,
                (unsigned)uc->uc_mcontext.arm_r10,
                (unsigned)uc->uc_mcontext.arm_fp,
                (unsigned)uc->uc_mcontext.arm_ip,
                (unsigned)uc->uc_mcontext.arm_sp,
                (unsigned)uc->uc_mcontext.arm_lr,
                (unsigned)uc->uc_mcontext.arm_pc,
                (unsigned)uc->uc_mcontext.arm_cpsr);
            write(fd, buf, len);

            /* Dynrec attribution. The generated blocks live in anonymous
             * mmap'd memory, so without this a fault inside one shows up
             * as a bare PC with no symbol and no way to tell it apart from
             * a fault inside a helper. Dump the block that was executing
             * and the ARM words around the faulting PC. */
            {
                uintptr_t cc_base = 0;
                uint32_t cc_size = 0;
                uint32_t arm_off = 0, code_len = 0, x86_pc = 0;
                unsigned pc = (unsigned)uc->uc_mcontext.arm_pc;

                dynrec_get_code_cache(&cc_base, &cc_size);
                dynrec_get_current_block(&arm_off, &code_len, &x86_pc);

                len = snprintf(buf, sizeof(buf),
                    "DYNREC: cache=[%p..%p) active x86_pc=0x%05X arm_off=%u code_len=%u\n",
                    (void*)cc_base, (void*)(cc_base + cc_size),
                    x86_pc, arm_off, code_len);
                write(fd, buf, len);

                /* entry_cpu is the cpu pointer dynrec_exec passed to
                 * func(cpu), sampled from C and therefore trustworthy. The
                 * cpu pointer is the ARGUMENT carried in R0 at each helper
                 * call site (the prologue `mov r4, r0` copies it into the
                 * block's R4, but R4 is a callee-saved register that every
                 * C helper — including cpu_exec itself — is free to reuse for
                 * its own locals, so fault-time R4 is NOT the block's R4 and
                 * is usually just the guest x86_pc). Check fault-time R0
                 * (== entry_cpu) to decide whether the cpu pointer survived
                 * the trip into the helper. caller_r4 is the C caller's R4 and
                 * is printed only for reference. */
                {
                    uint32_t entry_cpu = 0, caller_r4 = 0;
                    uint32_t fault_r0 = (uint32_t)uc->uc_mcontext.arm_r0;
                    uint32_t fault_r4 = (uint32_t)uc->uc_mcontext.arm_r4;
                    const char *verdict;

                    dynrec_get_current_entry(&entry_cpu, &caller_r4);

                    if (entry_cpu == 0) {
                        verdict = "no dynrec block was active";
                    } else if (entry_cpu < 0x00100000u || entry_cpu > 0xF0000000u) {
                        /* A .bss global such as &machine.CPU is in a mapped
                         * .so image, i.e. well above 1MB on 32-bit Android;
                         * anything under 1MB is an x86_pc or truncated value,
                         * not a CPU_t. */
                        verdict = "block was entered with a bogus cpu pointer "
                                  "(caller side)";
                    } else if (fault_r0 == entry_cpu) {
                        verdict = "cpu pointer intact (fault-time R0 == "
                                  "entry_cpu)";
                    } else {
                        verdict = "cpu pointer corrupted between block entry "
                                  "and fault (R0 != entry_cpu)";
                    }

                    len = snprintf(buf, sizeof(buf),
                        "  entry_cpu=0x%08X fault_r0=0x%08X fault_r4=0x%08X "
                        "caller_r4=0x%08X (C caller's R4, not the block's) -> %s\n",
                        entry_cpu, fault_r0, fault_r4, caller_r4, verdict);
                    write(fd, buf, len);
                }

                /* LR/SP classification. arm_lr separates a fault that
                 * happened inside a helper called from a generated block
                 * (LR points back into the code cache) from a fault that
                 * is not in generated code at all (LR follows the ordinary
                 * C call chain inside libxtulator.so). arm_sp alignment is
                 * an AAPCS invariant; combined with a live block + an
                 * in-cache LR, a misaligned SP directly implicates a helper
                 * that disturbed SP so the block's pop {r4,r10,r11,pc}
                 * reloaded a garbage R4 from the wrong stack slot. */
                {
                    unsigned arm_sp = (unsigned)uc->uc_mcontext.arm_sp;
                    unsigned arm_lr = (unsigned)uc->uc_mcontext.arm_lr;
                    const char *lr_where;
                    int in_cache = (cc_base && cc_size &&
                                    arm_lr >= (unsigned)cc_base &&
                                    arm_lr < (unsigned)(cc_base + cc_size));
                    int in_so = 0;
                    int maps_fd = open("/proc/self/maps", O_RDONLY);
                    if (maps_fd >= 0) {
                        FILE *mf_maps = fdopen(maps_fd, "r");
                        if (mf_maps) {
                            char mline[512];
                            while (fgets(mline, sizeof(mline), mf_maps)) {
                                unsigned long lo_m = 0, hi_m = 0;
                                if (sscanf(mline, "%lx-%lx", &lo_m, &hi_m) == 2 &&
                                    arm_lr >= (unsigned)lo_m &&
                                    arm_lr < (unsigned)hi_m) {
                                    if (strstr(mline, "libxtulator.so")) {
                                        in_so = 1;
                                    }
                                    break;
                                }
                            }
                            fclose(mf_maps);
                        } else {
                            close(maps_fd);
                        }
                    }
                    if (in_cache) {
                        lr_where = "inside code cache";
                    } else if (in_so) {
                        lr_where = "inside libxtulator.so";
                    } else {
                        lr_where = "neither (not in cache, not in libxtulator.so)";
                    }
                    {
                        int sp_aligned = (arm_sp & 7u) == 0;
                        int block_active = (arm_off != 0xFFFFFFFFu && code_len > 0);
                        int sp_implicated = (in_cache && block_active && !sp_aligned);
                        len = snprintf(buf, sizeof(buf),
                            "  LR/SP: arm_lr=0x%08X (%s), arm_sp=0x%08X %s, "
                            "block_active=%s, sp_unaligned_in_cache=%s\n",
                            arm_lr, lr_where, arm_sp,
                            sp_aligned ? "8-byte-aligned" : "NOT 8-byte-aligned",
                            block_active ? "yes" : "no",
                            sp_implicated ? "yes (implicates SP imbalance)" : "no");
                        write(fd, buf, len);
                    }
                }

                if (cc_base && cc_size && pc >= cc_base && pc < cc_base + cc_size) {
                    /* Word-aligned window centred on the fault. */
                    uint32_t *w = (uint32_t *)(cc_base + ((pc - cc_base) & ~3u));
                    int i;
                    len = snprintf(buf, sizeof(buf),
                        "  fault is in generated code; ARM words around PC:\n");
                    write(fd, buf, len);
                    for (i = -4; i <= 4; i++) {
                        len = snprintf(buf, sizeof(buf), "   %c %p: %08x\n",
                            (i == 0) ? '>' : ' ', (void *)(w + i), w[i]);
                        write(fd, buf, len);
                    }
                } else if (cc_base && cc_size) {
                    len = snprintf(buf, sizeof(buf),
                        "  fault PC is NOT in the code cache; faulted in a library\n");
                    write(fd, buf, len);
                    dump_maps_line_for_pc(fd, pc);
                }

                /* A block can fault inside a helper (fault PC in the .so)
                 * while still having published arm_off/code_len. Dumping the
                 * active block is then the only way to see the ARM code that
                 * called the helper — do it regardless of where PC landed. */
                if (cc_base && cc_size && arm_off != 0xFFFFFFFFu && code_len > 0) {
                    enum { DUMP_MAX = 64 };  /* 64 words == 256 bytes */
                    uint32_t nwords = code_len / 4;
                    uint32_t truncated = 0;
                    uint32_t i;
                    uint32_t *w;

                    if (nwords > DUMP_MAX) {
                        nwords = DUMP_MAX;
                        truncated = 1;
                    }
                    w = (uint32_t *)(cc_base + arm_off);
                    len = snprintf(buf, sizeof(buf),
                        "  active block at %p, %u bytes (dumping %u words as off word):\n",
                        (void *)(cc_base + arm_off), code_len, nwords);
                    write(fd, buf, len);
                    for (i = 0; i < nwords; i++) {
                        len = snprintf(buf, sizeof(buf), "   %04X %08X\n", i * 4, w[i]);
                        write(fd, buf, len);
                    }
                     if (truncated) {
                         len = snprintf(buf, sizeof(buf),
                             "  (block dump TRUNCATED at %u words of %u bytes)\n",
                             nwords, code_len);
                         write(fd, buf, len);
                     }
                 }

                 /* Guest bytes at the faulting x86_pc. The active block was
                  * translating/executing the guest instruction that lives here;
                  * printing its machine code is what pins down which opcode the
                  * interpreter was emulating when the native control transfer to
                  * fault_addr happened. */
                 if (x86_pc) {
                     uint8_t gb[8];
                     int gn = dynrec_get_guest_bytes(x86_pc, sizeof(gb), gb);
                     int gi;
                     len = snprintf(buf, sizeof(buf),
                         "  guest bytes at x86_pc=0x%05X:", x86_pc);
                     write(fd, buf, len);
                     for (gi = 0; gi < gn; gi++) {
                         len = snprintf(buf, sizeof(buf), " %02X", gb[gi]);
                         write(fd, buf, len);
                     }
                     if (gn == 0) {
                         len = snprintf(buf, sizeof(buf),
                             " (unmapped/MIO at 0x%05X)", x86_pc);
                     } else {
                         len = snprintf(buf, sizeof(buf), " (OK)");
                     }
                     write(fd, buf, len);
                  }
             }

             /* Native backtrace: no DWARF in this build, so scan the stack
              * for values that look like return addresses (this module's
              * text, the JIT code cache, or bionic/libc) to reconstruct the
              * call chain that reached the faulting native PC. */
             {
                 uintptr_t bt_base = 0; uint32_t bt_size = 0;
                 dynrec_get_code_cache(&bt_base, &bt_size);
                 unsigned bsp = (unsigned)uc->uc_mcontext.arm_sp;
                 unsigned bpc = (unsigned)uc->uc_mcontext.arm_pc;
                 unsigned i;
                 len = snprintf(buf, sizeof(buf),
                     "NATIVE_BT: pc=0x%08X sp=0x%08X lr=0x%08X\n",
                     bpc, bsp, (unsigned)uc->uc_mcontext.arm_lr);
                 write(fd, buf, len);
                 for (i = 0; i < 48; i++) {
                     unsigned word = *((volatile unsigned*)(bsp + (i * 4)));
                     if ((word >= 0x41f00000u && word <= 0x42100000u) ||
                         (bt_base && word >= (unsigned)bt_base &&
                          word < (unsigned)(bt_base + bt_size)) ||
                         (word >= 0xb0000000u && word <= 0xc0000000u)) {
                         len = snprintf(buf, sizeof(buf),
                             "  [%2u] 0x%08X\n", i, word);
                         write(fd, buf, len);
                     }
                 }
             }
         }

         close(fd);
     }

    signal(sig, SIG_DFL);
    raise(sig);
}

static void install_crash_handlers(void) {
    struct sigaction sa;
    sa.sa_sigaction = crash_signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_SIGINFO | SA_ONESHOT;

    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGABRT, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
    sigaction(SIGTRAP, &sa, NULL);
}

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

	install_crash_handlers();

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
