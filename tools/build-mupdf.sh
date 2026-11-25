#!/bin/sh
set -eu

cd "$(dirname "$0")/.."
root=$PWD
target=$1
out="$root/build/$target/mupdf"
mkdir -p "$out"
case "$target" in
    linux)
        set -- OS=Linux
        ;;
    windows)
        set -- OS=Windows CC=x86_64-w64-mingw32-gcc CXX=x86_64-w64-mingw32-g++ AR=x86_64-w64-mingw32-ar "XCFLAGS=-msse4.1 -D__USE_MINGW_ANSI_STDIO=1"
        ;;
    web)
        set -- OS=wasm
        ;;
    android-*)
        ndk=$2
        triple=$3
        llvm="$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin"
        set -- OS=Android CC="$llvm/${triple}29-clang" CXX="$llvm/${triple}29-clang++" AR="$llvm/llvm-ar" XCFLAGS=-fPIC
        ;;
    *)
        printf 'Unknown MuPDF target %s\n' "$target" >&2
        exit 2
        ;;
esac

# Build the bundled codecs and the PDF renderer. The viewer does not execute PDF JavaScript.
make -C "$root/.deps/mupdf-1.28.6-source" -j"${JOBS:-4}" \
    "OUT=$out" build=release html=no xps=no svg=no extract=no mujs=no \
    USE_SYSTEM_LIBS=no HAVE_LIBCRYPTO=no HAVE_OBJCOPY=no "$@" libs
