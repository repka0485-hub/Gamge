#!/bin/sh
# build_overlay.sh - build libesp_overlay.so with the Android NDK (run on the dev machine).
# Usage: ANDROID_NDK_HOME=/path/to/ndk sh build_overlay.sh
set -eu
: "${ANDROID_NDK_HOME:?set ANDROID_NDK_HOME}"
HOST="${NDK_HOST:-linux-x86_64}"   # FILL: linux-x86_64 | darwin-x86_64 | windows-x86_64
TC="$ANDROID_NDK_HOME/toolchains/llvm/prebuilt/$HOST"
CXX="$TC/bin/aarch64-linux-android24-clang++"
HERE="$(cd "$(dirname "$0")" && pwd)"

"$CXX" -shared -fPIC -O2 -std=c++17 -fvisibility=hidden \
  "$HERE/esp_overlay.cpp" -o "$HERE/libesp_overlay.so" \
  -llog -lEGL -lGLESv2 -ldl

cp "$TC/sysroot/usr/lib/aarch64-linux-android/libc++_shared.so" "$HERE/libc++_shared.so"
echo "built: $HERE/libesp_overlay.so"
echo "check: readelf -d libesp_overlay.so | grep NEEDED   (expect libc++_shared.so, liblog, libEGL, libGLESv2)"
