LOCAL_PATH := $(call my-dir)

include $(CLEAR_VARS)

LOCAL_MODULE := xtulator

LOCAL_SRC_FILES := \
        ../../XTulator/args.c \
        ../../XTulator/debuglog.c \
        ../../XTulator/machine.c \
        ../../XTulator/memory.c \
        ../../XTulator/ports.c \
        ../../XTulator/rtc.c \
        ../../XTulator/timing.c \
        ../../XTulator/utility.c \
        ../../XTulator/chipset/i8237.c \
        ../../XTulator/chipset/i8253.c \
        ../../XTulator/chipset/i8255.c \
        ../../XTulator/chipset/i8259.c \
        ../../XTulator/chipset/uart.c \
        ../../XTulator/cpu/cpu.c \
        ../../XTulator/cpu/dynrec.c \
        ../../XTulator/cpu/dynrec_helpers.c \
        ../../XTulator/modules/audio/blaster.c \
        ../../XTulator/modules/audio/nukedopl.c \
        ../../XTulator/modules/audio/opl2.c \
        ../../XTulator/modules/audio/pcspeaker.c \
        ../../XTulator/modules/disk/biosdisk.c \
        ../../XTulator/modules/disk/fdc.c \
        ../../XTulator/modules/input/mouse.c \
        ../../XTulator/modules/video/cga.c \
        ../../XTulator/modules/video/vga.c \
        android_frontend.c \
        android_audio.c \
        jni_bridge.c

LOCAL_C_INCLUDES := $(LOCAL_PATH)/../../XTulator

# Stage 22 churn probe: to log block translate/evict/kill for 0x0660D-0x06629,
# add -DDR_CHURN_PROBE to LOCAL_CFLAGS below (logcat tag XT-CHURN). Off by default.
LOCAL_CFLAGS := -D__ANDROID__ -O2 -g -std=gnu99

# Stage 24: target-core selection. The IS01 is an ARMv7 Scorpion with NEON
# (cpuinfo: implementer 0x51, part 0x00f). XT_ARMV7=1 builds for it:
#   ndk-build XT_ARMV7=1      (or export XT_ARMV7=1 before build-apk.sh)
# XT_ARMV7=0 (default) keeps the original ARMv5TE build for older devices.
# No NEON code is generated here (-O2 does not auto-vectorise), so this
# change only tests the core/ISA gain.
XT_ARMV7 ?= 0
ifeq ($(XT_ARMV7),1)
LOCAL_CFLAGS += -march=armv7-a -mtune=cortex-a8 -mfpu=neon -mfloat-abi=softfp
else
LOCAL_CFLAGS += -march=armv5te
endif

LOCAL_ARM_MODE := arm

LOCAL_LDLIBS := -llog -lm

include $(BUILD_SHARED_LIBRARY)
