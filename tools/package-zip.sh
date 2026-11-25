#!/bin/sh
set -eu

cd "$(dirname "$0")/.."
root=$PWD
case "${1:-}" in
    windows)
        name=folio-windows-x86_64
        set -- build/windows/folio.exe
        ;;
    web)
        name=folio-web
        set -- build/web/index.html build/web/index.js build/web/index.wasm
        ;;
    *)
        printf 'Usage: %s windows|web\n' "$0" >&2
        exit 2
        ;;
esac

mkdir -p "$root/dist"
work=$(mktemp -d)
trap 'find "$work" -depth -delete' EXIT
mkdir "$work/licenses"
for file in "$@"; do
    cp "$file" "$work/"
done
cp .deps/mupdf-1.28.6-source/COPYING "$work/licenses/MuPDF.txt"
cp .deps/mupdf-1.28.6-source/resources/fonts/noto/COPYING "$work/licenses/Noto.txt"
cp .deps/raylib-6.2/LICENSE "$work/licenses/raylib.txt"
(
    cd "$work"
    zip -q -r "$work/$name.zip" . -x "$name.zip"
)
mv -f "$work/$name.zip" "$root/dist/$name.zip"
printf '%s\n' "$root/dist/$name.zip"
