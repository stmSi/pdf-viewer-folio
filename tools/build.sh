#!/bin/sh
set -eu

cd "$(dirname "$0")/.."
root=$PWD
target=${1:-linux}
jobs=${JOBS:-4}

case "$target" in
    linux|windows|web|android) ;;
    *)
        printf 'Usage: %s linux|windows|web|android\n' "$0" >&2
        exit 2
        ;;
esac

./tools/dependencies.sh

build() {
    build_target=$1
    shift
    cmake -S "$root" -B "$root/build/$build_target" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        "-DMUPDF_BUILD_DIR=$root/build/$build_target/mupdf" "$@"
    cmake --build "$root/build/$build_target" --parallel "$jobs"
}

case "$target" in
    linux)
        ./tools/build-mupdf.sh linux
        build linux
        ;;
    windows)
        ./tools/build-mupdf.sh windows
        build windows "-DCMAKE_TOOLCHAIN_FILE=$root/platform/windows/mingw.cmake"
        ;;
    web)
        if ! command -v emcc >/dev/null 2>&1; then
            printf 'Activate the Emscripten SDK before building the web target.\n' >&2
            exit 1
        fi
        ./tools/build-mupdf.sh web
        emcmake cmake -S "$root" -B "$root/build/web" -G Ninja \
            -DCMAKE_BUILD_TYPE=Release "-DMUPDF_BUILD_DIR=$root/build/web/mupdf"
        cmake --build "$root/build/web" --parallel "$jobs"
        ;;
    android)
        sdk=${ANDROID_HOME:-${ANDROID_SDK_ROOT:-}}
        if [ -z "$sdk" ]; then
            printf 'Set ANDROID_HOME to the Android SDK directory.\n' >&2
            exit 1
        fi
        ndk=${ANDROID_NDK_HOME:-"$sdk/ndk/30.0.16248370"}
        if [ ! -f "$ndk/build/cmake/android.toolchain.cmake" ]; then
            printf 'Android NDK is missing at %s\n' "$ndk" >&2
            exit 1
        fi
        abis=${ANDROID_ABIS:-"arm64-v8a x86_64"}
        for abi in $abis; do
            case "$abi" in
                arm64-v8a) triple=aarch64-linux-android ;;
                x86_64) triple=x86_64-linux-android ;;
                *) printf 'Unsupported Android ABI %s\n' "$abi" >&2; exit 2 ;;
            esac
            ./tools/build-mupdf.sh "android-$abi" "$ndk" "$triple"
            build "android-$abi" "-DCMAKE_TOOLCHAIN_FILE=$ndk/build/cmake/android.toolchain.cmake" \
                "-DANDROID_ABI=$abi" -DANDROID_PLATFORM=android-29 -DANDROID_STL=none
        done
        ./tools/android-apk.sh "$sdk" $abis
        ;;
esac
