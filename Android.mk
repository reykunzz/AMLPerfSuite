LOCAL_PATH := $(call my-dir)

# Lokasi source AndroidModLoader (berisi folder mod/ dengan amlmod.h).
# Diisi otomatis oleh setup-aml-headers.sh, atau override: ndk-build AML_ROOT=/path/ke/AndroidModLoader
AML_ROOT ?= $(LOCAL_PATH)/third_party/AndroidModLoader

include $(CLEAR_VARS)

LOCAL_MODULE    := AMLPerfSuite
LOCAL_SRC_FILES := src/main.cpp src/config.cpp src/perf.cpp src/overlay.cpp src/menu.cpp

# Implementasi `logger` / Logger::Info dkk. ada di source AML (mod/logger.cpp),
# header-nya saja tidak cukup: harus ikut dikompilasi ke plugin.
AML_LOGGER := $(firstword $(wildcard $(AML_ROOT)/mod/logger.cpp) $(wildcard $(LOCAL_PATH)/include/mod/logger.cpp))
ifeq ($(AML_LOGGER),)
$(warning mod/logger.cpp tidak ditemukan di AML_ROOT=$(AML_ROOT); link akan gagal)
else
LOCAL_SRC_FILES += $(subst $(LOCAL_PATH)/,,$(AML_LOGGER))
endif

LOCAL_C_INCLUDES := $(LOCAL_PATH)/include \
                    $(LOCAL_PATH)/src \
                    $(AML_ROOT) \
                    $(AML_ROOT)/include \
                    $(AML_ROOT)/external \
                    $(AML_ROOT)/ARMPatch

LOCAL_CPPFLAGS := -std=c++17 -O2 -Wall -Wno-unused-function
# Tambahkan -DAMLPERF_ANY_GAME bila package GTA SA kamu bukan com.rockstargames.gtasa
# LOCAL_CPPFLAGS += -DAMLPERF_ANY_GAME

# AML tidak perlu di-link: interface diambil saat runtime lewat dlopen("libAML.so").
LOCAL_LDLIBS := -llog -lEGL -lGLESv2 -ldl

include $(BUILD_SHARED_LIBRARY)
