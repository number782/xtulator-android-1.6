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

LOCAL_CFLAGS := -D__ANDROID__ -D__ARM_ARCH_5TE__ -O2 -g -std=gnu99

LOCAL_LDLIBS := -llog -lm

include $(BUILD_SHARED_LIBRARY)
