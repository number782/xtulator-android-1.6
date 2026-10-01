#include <stdint.h>
#include <pthread.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <jni.h>

#include "../XTulator/config.h"
#include "../XTulator/debuglog.h"
#include "../XTulator/modules/video/sdlconsole.h"
#include "android_frontend.h"

#ifdef __ANDROID__
#include <android/log.h>
#define ANDROID_LOGI(...) do { if (trace_flags & TRACE_FLAG_VIDEO) { __android_log_print(ANDROID_LOG_INFO, "XTulator-ANDROID", __VA_ARGS__); } } while (0)
#else
#define ANDROID_LOGI(...)
#endif

#define FB_MAX_W 1024
#define FB_MAX_H 1024
#define FB_CAP   (FB_MAX_W * FB_MAX_H)

static uint32_t *g_fb = NULL;
static int g_fbW = 0;
static int g_fbH = 0;
static pthread_mutex_t g_fbMtx = PTHREAD_MUTEX_INITIALIZER;

static uint8_t g_lastScancode = 0;

uint8_t android_keycode_to_scancode(int keycode)
{
	switch (keycode) {
	case 111: return 0x01; /* ESC */
	case 68:  return 0x29; /* GRAVE (`) */
	case 7:   return 0x0B; /* 0 */
	case 8:   return 0x02; /* 1 */
	case 9:   return 0x03; /* 2 */
	case 10:  return 0x04; /* 3 */
	case 11:  return 0x05; /* 4 */
	case 12:  return 0x06; /* 5 */
	case 13:  return 0x07; /* 6 */
	case 14:  return 0x08; /* 7 */
	case 15:  return 0x09; /* 8 */
	case 16:  return 0x0A; /* 9 */
	case 69:  return 0x0C; /* MINUS (-) */
	case 70:  return 0x0D; /* EQUALS (=) */
	case 67:  return 0x0E; /* BACKSPACE (DEL) */
	case 61:  return 0x0F; /* TAB */
	case 45:  return 0x10; /* Q */
	case 51:  return 0x11; /* W */
	case 33:  return 0x12; /* E */
	case 46:  return 0x13; /* R */
	case 48:  return 0x14; /* T */
	case 53:  return 0x15; /* Y */
	case 49:  return 0x16; /* U */
	case 37:  return 0x17; /* I */
	case 43:  return 0x18; /* O */
	case 44:  return 0x19; /* P */
	case 71:  return 0x1A; /* LEFT_BRACKET [ */
	case 72:  return 0x1B; /* RIGHT_BRACKET ] */
	case 66:  return 0x1C; /* ENTER */
	case 113: return 0x1D; /* CTRL_LEFT */
	case 114: return 0x1D; /* CTRL_RIGHT */
	case 29:  return 0x1E; /* A */
	case 47:  return 0x1F; /* S */
	case 32:  return 0x20; /* D */
	case 34:  return 0x21; /* F */
	case 35:  return 0x22; /* G */
	case 36:  return 0x23; /* H */
	case 38:  return 0x24; /* J */
	case 39:  return 0x25; /* K */
	case 40:  return 0x26; /* L */
	case 74:  return 0x27; /* SEMICOLON ; */
	case 75:  return 0x28; /* APOSTROPHE ' */
	case 59:  return 0x2A; /* SHIFT_LEFT */
	case 60:  return 0x36; /* SHIFT_RIGHT */
	case 73:  return 0x2B; /* BACKSLASH \ */
	case 54:  return 0x2C; /* Z */
	case 52:  return 0x2D; /* X */
	case 31:  return 0x2E; /* C */
	case 50:  return 0x2F; /* V */
	case 30:  return 0x30; /* B */
	case 42:  return 0x31; /* N */
	case 41:  return 0x32; /* M */
	case 55:  return 0x33; /* COMMA , */
	case 56:  return 0x34; /* PERIOD . */
	case 76:  return 0x35; /* SLASH / */
	case 57:  return 0x38; /* ALT_LEFT */
	case 58:  return 0x38; /* ALT_RIGHT */
	case 62:  return 0x39; /* SPACE */
	case 115: return 0x3A; /* CAPS_LOCK */
	case 131: return 0x3B; /* F1 */
	case 132: return 0x3C; /* F2 */
	case 133: return 0x3D; /* F3 */
	case 134: return 0x3E; /* F4 */
	case 135: return 0x3F; /* F5 */
	case 136: return 0x40; /* F6 */
	case 137: return 0x41; /* F7 */
	case 138: return 0x42; /* F8 */
	case 139: return 0x43; /* F9 */
	case 140: return 0x44; /* F10 */
	case 141: return 0x57; /* F11 */
	case 142: return 0x58; /* F12 */
	case 19:  return 0x48; /* DPAD_UP */
	case 20:  return 0x50; /* DPAD_DOWN */
	case 21:  return 0x4B; /* DPAD_LEFT */
	case 22:  return 0x4D; /* DPAD_RIGHT */
	case 23:  return 0x1C; /* DPAD_CENTER -> ENTER */
	case 112: return 0x0E; /* FORWARD_DEL -> BACKSPACE */
	case 6:   return 0x01; /* ENDCALL -> ESC */
	case 82:  return 0x01; /* MENU -> ESC */
	case 3:   return 0x01; /* HOME -> ESC */
	case 4:   return 0x01; /* BACK -> ESC */
	case 122: return 0x47; /* MOVE_HOME -> HOME */
	case 123: return 0x4F; /* MOVE_END -> END */
	case 92:  return 0x49; /* PAGE_UP */
	case 93:  return 0x51; /* PAGE_DOWN */
	default:  return 0;    /* unknown / unmapped (ignored) */
	}
}

int sdlconsole_init(char *title)
{
	ANDROID_LOGI("android sdlconsole_init: start");
	(void)title;
	if (g_fb != NULL) {
		ANDROID_LOGI("android sdlconsole_init: already initialized");
		return 0;
	}
	g_fb = (uint32_t *)calloc((size_t)FB_CAP, sizeof(uint32_t));
	ANDROID_LOGI("android sdlconsole_init: after calloc, g_fb=%p", g_fb);
	if (g_fb == NULL) {
		ANDROID_LOGI("android sdlconsole_init: calloc failed");
		return -1;
	}
	g_fbW = 0;
	g_fbH = 0;
	g_lastScancode = 0;
	ANDROID_LOGI("android sdlconsole_init: returning 0");
	return 0;
}

void sdlconsole_blit(uint32_t *pixels, int w, int h, int stride)
{
	if (pixels == NULL || g_fb == NULL || w <= 0 || h <= 0) {
		ANDROID_LOGI("sdlconsole_blit: early return, pixels=%p, g_fb=%p, w=%d, h=%d", pixels, g_fb, w, h);
		return;
	}
	// Check first few pixels
	ANDROID_LOGI("sdlconsole_blit: w=%d, h=%d, first pixel=0x%08X, last pixel=0x%08X", 
		w, h, pixels[0], pixels[w*h-1]);
	int rowUintStride = stride / 4;
	if (rowUintStride < w) {
		rowUintStride = w;
	}

	pthread_mutex_lock(&g_fbMtx);
	size_t total = (size_t)w * (size_t)h;
	size_t n = total > (size_t)FB_CAP ? (size_t)FB_CAP : total;
	const size_t rowPixels = (size_t)w;
	uint32_t *dst = g_fb;
	for (int y = 0; y < h && n > 0; y++) {
		size_t cp = rowPixels > n ? n : rowPixels;
		memcpy(dst, pixels + (size_t)y * (size_t)rowUintStride,
		       cp * sizeof(uint32_t));
		dst += cp;
		n -= cp;
	}
	g_fbW = w;
	g_fbH = h;
	pthread_mutex_unlock(&g_fbMtx);
}

int sdlconsole_loop(void)
{
	return SDLCONSOLE_EVENT_NONE;
}

uint8_t sdlconsole_getScancode(void)
{
	uint8_t sc;
	pthread_mutex_lock(&g_fbMtx);
	sc = g_lastScancode;
	pthread_mutex_unlock(&g_fbMtx);
	return sc;
}

uint8_t sdlconsole_translateScancode(int keyval)
{
	uint8_t sc = android_keycode_to_scancode(keyval);
	pthread_mutex_lock(&g_fbMtx);
	if (sc != 0) {
		g_lastScancode = sc;
	}
	pthread_mutex_unlock(&g_fbMtx);
	return sc;
}

int sdlconsole_setWindow(int w, int h)
{
	(void)w;
	(void)h;
	return 0;
}

void sdlconsole_setTitle(char *title)
{
	(void)title;
}

void android_fb_get_dims(int *w, int *h)
{
	if (w == NULL || h == NULL) {
		return;
	}
	pthread_mutex_lock(&g_fbMtx);
	*w = g_fbW;
	*h = g_fbH;
	ANDROID_LOGI("android_fb_get_dims: returning w=%d, h=%d", g_fbW, g_fbH);
	pthread_mutex_unlock(&g_fbMtx);
}

int android_fb_copy(int32_t *out, int count)
{
	if (out == NULL || g_fb == NULL || count <= 0) {
		ANDROID_LOGI("android_fb_copy: early return, out=%p, g_fb=%p, count=%d", out, g_fb, count);
		return 0;
	}
	pthread_mutex_lock(&g_fbMtx);
	int total = g_fbW * g_fbH;
	ANDROID_LOGI("android_fb_copy: g_fbW=%d, g_fbH=%d, total=%d, count=%d", g_fbW, g_fbH, total, count);
	int n = count < total ? count : total;
	for (int i = 0; i < n; i++) {
		out[i] = (int32_t)(g_fb[i] | 0xFF000000u);
	}
pthread_mutex_unlock(&g_fbMtx);
    return n;
}

#ifdef __ANDROID__
/*
	Asset copying using JNI calls to Java's AssetManager API.
	This works on API level 4 (Android 1.6) because it uses the Java API
	instead of the NDK's AAssetManager_fromJava which requires API 9+.
*/
int android_copy_asset(void *env, void *assetManager, const char *assetName, const char *outPath)
{
    JNIEnv *jniEnv = (JNIEnv *)env;
    jobject jAssetManager = (jobject)assetManager;
    if (!jniEnv || !jAssetManager) {
        ANDROID_LOGI("android_copy_asset: null env or assetManager");
        return -1;
    }

    // Get AssetManager class
    jclass assetManagerClass = (*jniEnv)->GetObjectClass(jniEnv, jAssetManager);
    if (!assetManagerClass) {
        ANDROID_LOGI("android_copy_asset: failed to get AssetManager class");
        return -1;
    }

    // Get open() method: InputStream open(String fileName)
    jmethodID openMethod = (*jniEnv)->GetMethodID(jniEnv, assetManagerClass, "open", "(Ljava/lang/String;)Ljava/io/InputStream;");
    if (!openMethod) {
        ANDROID_LOGI("android_copy_asset: failed to get open method");
        return -1;
    }

    // Convert assetName to jstring
    jstring jAssetName = (*jniEnv)->NewStringUTF(jniEnv, assetName);
    if (!jAssetName) {
        ANDROID_LOGI("android_copy_asset: failed to create jstring for asset name");
        return -1;
    }

    // Call open()
    jobject inputStream = (*jniEnv)->CallObjectMethod(jniEnv, jAssetManager, openMethod, jAssetName);
    (*jniEnv)->DeleteLocalRef(jniEnv, jAssetName);
    if ((*jniEnv)->ExceptionCheck(jniEnv)) {
        ANDROID_LOGI("android_copy_asset: exception during open for '%s'", assetName);
        (*jniEnv)->ExceptionClear(jniEnv);
        return -1;
    }
    if (!inputStream) {
        ANDROID_LOGI("android_copy_asset: failed to open asset '%s'", assetName);
        return -1;
    }

    // Get InputStream class
    jclass inputStreamClass = (*jniEnv)->GetObjectClass(jniEnv, inputStream);
    if (!inputStreamClass) {
        ANDROID_LOGI("android_copy_asset: failed to get InputStream class");
        (*jniEnv)->DeleteLocalRef(jniEnv, inputStream);
        return -1;
    }

    // Get read() method: int read(byte[] buffer)
    jmethodID readMethod = (*jniEnv)->GetMethodID(jniEnv, inputStreamClass, "read", "([B)I");
    if (!readMethod) {
        ANDROID_LOGI("android_copy_asset: failed to get read method");
        (*jniEnv)->DeleteLocalRef(jniEnv, inputStream);
        return -1;
    }

    // Get close() method
    jmethodID closeMethod = (*jniEnv)->GetMethodID(jniEnv, inputStreamClass, "close", "()V");
    if (!closeMethod) {
        ANDROID_LOGI("android_copy_asset: failed to get close method");
        (*jniEnv)->DeleteLocalRef(jniEnv, inputStream);
        return -1;
    }

    // Open output file
    FILE *out = fopen(outPath, "wb");
    if (out == NULL) {
        ANDROID_LOGI("android_copy_asset: failed to open output '%s'", outPath);
        (*jniEnv)->DeleteLocalRef(jniEnv, inputStream);
        return -1;
    }

    // Create a byte buffer (8KB)
    jbyteArray buffer = (*jniEnv)->NewByteArray(jniEnv, 8192);
    if (!buffer) {
        ANDROID_LOGI("android_copy_asset: failed to create byte array");
        fclose(out);
        (*jniEnv)->DeleteLocalRef(jniEnv, inputStream);
        return -1;
    }

    int total = 0;
    int n;
    while (1) {
        n = (*jniEnv)->CallIntMethod(jniEnv, inputStream, readMethod, buffer);
        if ((*jniEnv)->ExceptionCheck(jniEnv)) {
            ANDROID_LOGI("android_copy_asset: exception during read for '%s'", assetName);
            (*jniEnv)->ExceptionClear(jniEnv);
            break;
        }
        if (n <= 0) break;

        // Get the bytes from the Java array
        jbyte *bytes = (*jniEnv)->GetByteArrayElements(jniEnv, buffer, NULL);
        if (bytes) {
            size_t written = fwrite(bytes, 1, n, out);
            (*jniEnv)->ReleaseByteArrayElements(jniEnv, buffer, bytes, JNI_ABORT);
            if (written != (size_t)n) {
                ANDROID_LOGI("android_copy_asset: fwrite failed, wrote %zu of %d", written, n);
                break;
            }
            total += n;
        } else {
            ANDROID_LOGI("android_copy_asset: GetByteArrayElements failed");
            break;
        }
    }

    // Clean up
    (*jniEnv)->CallVoidMethod(jniEnv, inputStream, closeMethod);
    if ((*jniEnv)->ExceptionCheck(jniEnv)) {
        (*jniEnv)->ExceptionClear(jniEnv);
    }
    (*jniEnv)->DeleteLocalRef(jniEnv, buffer);
    (*jniEnv)->DeleteLocalRef(jniEnv, inputStream);
    fclose(out);

    ANDROID_LOGI("android_copy_asset: copied %d bytes for '%s'", total, assetName);
    return total;
}
#else
int android_copy_asset(void *env, void *assetManager, const char *assetName, const char *outPath)
{
    (void)env; (void)assetManager; (void)assetName; (void)outPath;
    return -1;
}
#endif
