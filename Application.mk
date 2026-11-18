APP_ABI      := armeabi-v7a arm64-v8a
APP_PLATFORM := android-21
APP_STL      := c++_static
APP_CPPFLAGS += -std=c++17
# NDK r27+: .so kompatibel dengan perangkat page-size 16 KB (diabaikan oleh NDK lama)
APP_SUPPORT_FLEXIBLE_PAGE_SIZES := true
