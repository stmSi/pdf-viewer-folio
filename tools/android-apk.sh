#!/bin/sh
set -eu

cd "$(dirname "$0")/.."
root=$PWD
sdk=$1
shift
tools=${ANDROID_BUILD_TOOLS:-"$sdk/build-tools/36.0.0"}
if [ -n "${ANDROID_JAR:-}" ]; then
    android_jar=$ANDROID_JAR
else
    android_jar=$(python3 - "$sdk/platforms" <<'PY'
import pathlib, re, sys
jars = list(pathlib.Path(sys.argv[1]).glob('android-*/android.jar'))
print(max(jars, key=lambda p: tuple(map(int, re.findall(r'\d+', p.parent.name))), default=''))
PY
)
fi
if [ ! -f "$android_jar" ] || [ ! -x "$tools/aapt2" ]; then
    printf 'Install an Android SDK platform and build-tools 36.0.0.\n' >&2
    exit 1
fi

dir="$root/build/android-apk"
mkdir -p "$dir" "$root/dist"
work=$(mktemp -d "$dir/package.XXXXXX")
trap 'find "$work" -depth -delete' EXIT
mkdir -p "$work/classes" "$work/dex" "$work/assets/licenses"
cp .deps/mupdf-1.28.6-source/COPYING "$work/assets/licenses/MuPDF.txt"
cp .deps/mupdf-1.28.6-source/resources/fonts/noto/COPYING "$work/assets/licenses/Noto.txt"
cp .deps/raylib-6.2/LICENSE "$work/assets/licenses/raylib.txt"
javac --release 8 -classpath "$android_jar" -d "$work/classes" \
    "$root"/platform/android/src/org/folio/viewer/*.java
"$tools/d8" --lib "$android_jar" --min-api 29 --output "$work/dex" \
    "$work/classes"/org/folio/viewer/*.class
"$tools/aapt2" link -I "$android_jar" -A "$work/assets" \
    --manifest "$root/platform/android/AndroidManifest.xml" -o "$work/unsigned.apk"
cp "$work/dex/classes.dex" "$work/classes.dex"
(
    cd "$work"
    zip -q unsigned.apk classes.dex
    for abi in "$@"; do
        mkdir -p "lib/$abi"
        cp "$root/build/android-$abi/libfolio.so" "lib/$abi/libfolio.so"
        zip -q unsigned.apk "lib/$abi/libfolio.so"
    done
)
"$tools/zipalign" -P 16 4 "$work/unsigned.apk" "$work/aligned.apk"
keystore="$dir/debug.keystore"
if [ ! -f "$keystore" ]; then
    keytool -genkeypair -keystore "$keystore" -storepass android -keypass android \
        -alias androiddebugkey -dname 'CN=Android Debug,O=Android,C=US' \
        -keyalg RSA -keysize 2048 -validity 10000
fi
"$tools/apksigner" sign --ks "$keystore" --ks-pass pass:android --key-pass pass:android \
    --ks-key-alias androiddebugkey --v4-signing-enabled false \
    --out "$root/dist/folio-android-debug.apk" "$work/aligned.apk"
"$tools/apksigner" verify "$root/dist/folio-android-debug.apk"
printf '%s\n' "$root/dist/folio-android-debug.apk"
