#include "../XTulator/config.h"
#include "../XTulator/modules/audio/sdlaudio.h"
#include "../XTulator/modules/audio/pcspeaker.h"
#include "../XTulator/modules/audio/blaster.h"
#include "../XTulator/modules/audio/opl2.h"
#include "../XTulator/machine.h"
#include "../XTulator/timing.h"
#include "../XTulator/debuglog.h"

#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <jni.h>
#include <android/log.h>

#define SDLAUDIO_WRITE_CHUNK 1024

/*
	Catch-up budget for the sample timer, in samples per timing_loop() call.
	See sdlaudio_init() for why this is capped rather than tracking SAMPLE_RATE.
*/
#define SDLAUDIO_CATCHUP_MAX 192

/*
	The sample timer disables itself when the ring buffer fills, but nothing ever
	re-enabled it -- so one overrun permanently killed audio. Re-enable from the
	drain side with hysteresis (disable at 100%, re-enable at 25%) so the two
	cannot ping-pong, each transition costing a timer resync.
*/
#define SDLAUDIO_REARM_LOW_WATER 0.25

static int16_t ring[SAMPLE_BUFFER];
static int readIdx = 0;
static int writeIdx = 0;
static int count = 0;
static pthread_mutex_t ringMtx = PTHREAD_MUTEX_INITIALIZER;
static int sdlaudio_timer = -1;
static MACHINE_t* sdlaudio_useMachine = NULL;
static double sdlaudio_rateFast = 0.0;
static volatile int sdlaudio_updateTiming = 0;

static JavaVM* gJavaVM = NULL;
static jobject gAudioTrackObj = NULL;
static jmethodID gWriteAudioMethod = NULL;

#define AUDIO_LOGI(...) do { if (trace_flags & TRACE_FLAG_MISC) { __android_log_print(ANDROID_LOG_INFO, "XTulatorAudio", __VA_ARGS__); } } while (0)
#define AUDIO_LOGE(...) do { if (trace_flags & TRACE_FLAG_MISC) { __android_log_print(ANDROID_LOG_ERROR, "XTulatorAudio", __VA_ARGS__); } } while (0)

JNIEXPORT jint JNICALL JNI_OnLoad(JavaVM* vm, void* reserved) {
    gJavaVM = vm;
    if (trace_flags & TRACE_FLAG_MISC) __android_log_print(ANDROID_LOG_ERROR, "XTulator-JNI", "JNI_OnLoad called");
    return JNI_VERSION_1_4;
}

static void sdlaudio_ensureJavaRefs(JNIEnv* env) {
    if (gAudioTrackObj == NULL || gWriteAudioMethod == NULL) {
        jclass clazz = (*env)->FindClass(env, "com/xtulator/android/XTulatorActivity");
        if (clazz != NULL) {
            gWriteAudioMethod = (*env)->GetStaticMethodID(env, clazz, "writeAudioSamples", "([SII)I");
            if (gWriteAudioMethod == NULL) {
                AUDIO_LOGE("writeAudioSamples method not found");
            }
        } else {
            AUDIO_LOGE("XTulatorActivity class not found");
        }
    }
}

static int sdlaudio_writeToAudioTrack(int16_t* buffer, int frames) {
    JNIEnv* env = NULL;
    int needDetach = 0;
    int result = 0;

    if (gJavaVM == NULL) {
        return -1;
    }

    int getEnvResult = (*gJavaVM)->GetEnv(gJavaVM, (void**)&env, JNI_VERSION_1_4);
    if (getEnvResult == JNI_EDETACHED) {
        if ((*gJavaVM)->AttachCurrentThread(gJavaVM, &env, NULL) != JNI_OK) {
            return -1;
        }
        needDetach = 1;
    } else if (getEnvResult != JNI_OK) {
        return -1;
    }

    sdlaudio_ensureJavaRefs(env);

    if (gWriteAudioMethod != NULL) {
        jshortArray jBuffer = (*env)->NewShortArray(env, frames);
        if (jBuffer != NULL) {
            (*env)->SetShortArrayRegion(env, jBuffer, 0, frames, (const jshort*)buffer);
            result = (*env)->CallStaticIntMethod(env, NULL, gWriteAudioMethod, jBuffer, 0, frames);
            (*env)->DeleteLocalRef(env, jBuffer);
        }
    }

    if (needDetach) {
        (*gJavaVM)->DetachCurrentThread(gJavaVM);
    }

    return result;
}

int sdlaudio_init(MACHINE_t* machine) {
    sdlaudio_useMachine = machine;
	AUDIO_LOGI("android sdlaudio_init: start");
    sdlaudio_rateFast = (double)SAMPLE_RATE * 1.01;

    pthread_mutex_init(&ringMtx, NULL);
    readIdx = 0;
    writeIdx = 0;
    count = 0;
    memset(ring, 0, sizeof(ring));

    sdlaudio_timer = -1;
#if XT_ENABLE_AUDIO
    sdlaudio_timer = (int)timing_addTimer(sdlaudio_generateSample, NULL,
        (double)SAMPLE_RATE, TIMING_ENABLED);
    /*
        Cap how many samples a single timing_loop() call may generate.

        One call to OPL3_GenerateStream() runs a whole Nuked OPL3 sample, which
        is orders of magnitude more expensive than the PIT/video callbacks. Now
        that timing_loop() catches up on elapsed time, a faithful 48kHz sample
        rate would try to generate ~1000 samples per loop iteration and eat most
        of the CPU budget the 8088 core needs. Cap it, and let the ring buffer
        and the AudioTrack drain pace decide the real rate.
    */
    if (sdlaudio_timer >= 0) {
        timing_setCatchup((uint32_t)sdlaudio_timer, SDLAUDIO_CATCHUP_MAX);
    }
#else
    /*
        Audio disabled (XT_ENABLE_AUDIO == 0 in config.h). No sample timer is
        registered at all, so the emulator spends nothing on mixing. Everything
        downstream already guards on sdlaudio_timer >= 0, so the ring buffer
        simply stays empty and the Java side sees no data to write.
    */
#endif
	AUDIO_LOGI("android sdlaudio_init: returning 0");
    return 0;
}

void sdlaudio_bufferSample(int16_t val) {
    pthread_mutex_lock(&ringMtx);
    if (count < SAMPLE_BUFFER) {
        ring[writeIdx] = val;
        writeIdx = (writeIdx + 1) % SAMPLE_BUFFER;
        count++;
    }
    if (count < (int)((double)SAMPLE_BUFFER * 0.5)) {
        sdlaudio_updateTiming = SDLAUDIO_TIMING_FAST;
    } else if (count >= (int)((double)SAMPLE_BUFFER * 0.75)) {
        sdlaudio_updateTiming = SDLAUDIO_TIMING_NORMAL;
    }
    if (count == SAMPLE_BUFFER) {
        if (sdlaudio_timer >= 0) {
            timing_timerDisable(sdlaudio_timer);
        }
    }
    pthread_mutex_unlock(&ringMtx);
}

void sdlaudio_generateSample(void* dummy) {
    int16_t val;
    (void)dummy;

#if !XT_ENABLE_AUDIO
    return; /*audio disabled; see XT_ENABLE_AUDIO in config.h*/
#endif

    val = pcspeaker_getSample(&sdlaudio_useMachine->pcspeaker) / 3;
    if (sdlaudio_useMachine->mixOPL) {
        int16_t oplsample[2];
        OPL3_GenerateStream(&sdlaudio_useMachine->OPL3, oplsample, 1);
        val += oplsample[0] / 2;
    }
    if (sdlaudio_useMachine->mixBlaster) {
        val += blaster_getSample(&sdlaudio_useMachine->blaster) / 3;
    }
    sdlaudio_bufferSample(val);
}

void sdlaudio_updateSampleTiming(void) {
    if (sdlaudio_updateTiming == SDLAUDIO_TIMING_FAST) {
        if (sdlaudio_timer >= 0) {
            timing_updateIntervalFreq((uint32_t)sdlaudio_timer, sdlaudio_rateFast);
        }
    } else if (sdlaudio_updateTiming == SDLAUDIO_TIMING_NORMAL) {
        if (sdlaudio_timer >= 0) {
            timing_updateIntervalFreq((uint32_t)sdlaudio_timer, (double)SAMPLE_RATE);
        }
    }
    sdlaudio_updateTiming = 0;
}

JNIEXPORT void JNICALL Java_com_xtulator_android_XTulatorActivity_nativeFlushAudio(JNIEnv* env, jclass clazz) {
    int16_t chunk[SDLAUDIO_WRITE_CHUNK];
    int frames;

    pthread_mutex_lock(&ringMtx);
    while (count >= SDLAUDIO_WRITE_CHUNK) {
        for (int i = 0; i < SDLAUDIO_WRITE_CHUNK; i++) {
            chunk[i] = ring[readIdx];
            readIdx = (readIdx + 1) % SAMPLE_BUFFER;
            count--;
        }
        pthread_mutex_unlock(&ringMtx);
        sdlaudio_writeToAudioTrack(chunk, SDLAUDIO_WRITE_CHUNK);
        pthread_mutex_lock(&ringMtx);
    }
    /* Ring has drained: re-arm the sample timer if an overrun had disabled it. */
    if (sdlaudio_timer >= 0 && count <= (int)(SAMPLE_BUFFER * SDLAUDIO_REARM_LOW_WATER)) {
        timing_timerEnable((uint32_t)sdlaudio_timer);
    }
    pthread_mutex_unlock(&ringMtx);
}
