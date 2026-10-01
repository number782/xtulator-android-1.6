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

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#ifdef _WIN32
#include <Windows.h>
#else
#include <time.h>
#include <errno.h>
#endif
#include "config.h"
#include "memory.h"
#include "debuglog.h"
#include "diag.h"


#ifdef __ANDROID__
#include <android/log.h>
#define UTIL_LOGI(...) do { if (trace_flags & TRACE_FLAG_MISC) { __android_log_print(ANDROID_LOG_INFO, "XTulator-UTIL", __VA_ARGS__); } } while (0)
#else
#define UTIL_LOGI(...)
#endif


int utility_loadFile(uint8_t* dst, size_t len, char* srcfile) {
    FILE* file;
    if (trace_flags & TRACE_FLAG_MISC) __android_log_print(ANDROID_LOG_ERROR, "XTulator-UTILITY", "utility_loadFile: loading %s, len=%zu", srcfile, len);
    if (dst == NULL) {
        if (trace_flags & TRACE_FLAG_MISC) __android_log_print(ANDROID_LOG_ERROR, "XTulator-UTILITY", "utility_loadFile: dst is NULL");
        return -1;
    }

    file = fopen(srcfile, "rb");
    if (trace_flags & TRACE_FLAG_MISC) __android_log_print(ANDROID_LOG_ERROR, "XTulator-UTILITY", "utility_loadFile: fopen returned %p", (void*)file);
    if (file == NULL) {
        if (trace_flags & TRACE_FLAG_MISC) __android_log_print(ANDROID_LOG_ERROR, "XTulator-UTILITY", "utility_loadFile: fopen failed for %s", srcfile);
        return -1;
    }
        if (fread(dst, 1, len, file) < len) {
            if (trace_flags & TRACE_FLAG_MISC) __android_log_print(ANDROID_LOG_ERROR, "XTulator-UTILITY", "utility_loadFile: fread got %ld bytes, expected %d", (long)ftell(file), (int)len);
        fclose(file);
        return -1;
    }
    fclose(file);
    if (trace_flags & TRACE_FLAG_MISC) __android_log_print(ANDROID_LOG_ERROR, "XTulator-UTILITY", "utility_loadFile: success, first 8 bytes: %02X %02X %02X %02X %02X %02X %02X %02X", dst[0], dst[1], dst[2], dst[3], dst[4], dst[5], dst[6], dst[7]);
    return 0;
}

void utility_sleep(uint32_t ms) {
#ifdef _WIN32
    Sleep((DWORD)ms);
#else
    int res;
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = (long)ms * 1000;
    do {
        res = nanosleep(&ts, &ts);
    } while (res && errno == EINTR);
#endif
}
