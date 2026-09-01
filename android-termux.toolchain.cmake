# Cross-compilation toolchain for Android arm64 from an aarch64 Termux host.
# The official NDK ships x86_64 host tools that cannot run on aarch64 Termux,
# so this file uses Termux clang (targeting aarch64-linux-android) with the
# NDK sysroot. Set ANDROID_NDK_HOME before configuring.
set(CMAKE_SYSTEM_NAME Android)
set(CMAKE_SYSTEM_VERSION 23)
set(CMAKE_ANDROID_ARCH_ABI arm64-v8a)
set(ANDROID_ABI arm64-v8a)
set(CMAKE_ANDROID_NDK "$ENV{ANDROID_NDK_HOME}")
set(CMAKE_ANDROID_STL c++_shared)

set(NDK_SYSROOT "$ENV{ANDROID_NDK_HOME}/toolchains/llvm/prebuilt/linux-x86_64/sysroot")

set(CMAKE_C_COMPILER clang)
set(CMAKE_CXX_COMPILER clang++)
set(CMAKE_SYSROOT "${NDK_SYSROOT}")

set(CMAKE_C_FLAGS_INIT "--target=aarch64-linux-android23")
set(CMAKE_CXX_FLAGS_INIT "--target=aarch64-linux-android23")

set(CMAKE_FIND_ROOT_PATH "${NDK_SYSROOT}")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

# Android platform runtime libraries used by the shared backends.
set(CMAKE_SHARED_LINKER_FLAGS_INIT "-landroid -llog -ljnigraphics")
