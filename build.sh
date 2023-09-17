#!/bin/bash

ANDROID_NDK=/usr/local/android-ndk
HOST_TAG=linux-x86_64
ANDROID_TOOLCHAIN=$ANDROID_NDK/toolchains/llvm/prebuilt/$HOST_TAG
ANDROID_TOOLCHAIN_FILE=$ANDROID_NDK/build/cmake/android.toolchain.cmake
ANDROID_API=29
ANDROID_ARCH=arm64
ANDROID_CPU=armv8-a
LIB_TARGET_ABI=arm64-v8a
ANDROID_PLATFORM=android-$ANDROID_API

CC=$ANDROID_TOOLCHAIN/bin/aarch64-linux-android$ANDROID_API-clang
CXX=$ANDROID_TOOLCHAIN/bin/aarch64-linux-android$ANDROID_API-clang++

FFMPEG_DIR=/home/r/Scripts/C++/android_ffmpeg/ffmpeg-4.4.4/android/$ANDROID_CPU
FDK_AAC_DIR=/home/r/Scripts/C++/android_ffmpeg/fdk-aac-2.0.2/android/$LIB_TARGET_ABI
X264_DIR=/home/r/Scripts/C++/android_ffmpeg/x264/android/$LIB_TARGET_ABI

mkdir -p build && cd build && rm -rf *

cmake -DCMAKE_EXPORT_COMPILE_COMMANDS=1 \
      -DFFMPEG_DIR=$FFMPEG_DIR \
      -DFDK_AAC_DIR=$FDK_AAC_DIR \
      -DX264_DIR=$X264_DIR \
      -DCMAKE_TOOLCHAIN_FILE=$ANDROID_TOOLCHAIN_FILE \
      -DANDROID_ABI=$LIB_TARGET_ABI \
      -DANDROID_PLATFORM=$ANDROID_PLATFORM ..

make -j$(nproc)
