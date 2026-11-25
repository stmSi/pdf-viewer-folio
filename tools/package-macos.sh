#!/bin/sh
set -eu

cd "$(dirname "$0")/.."
mkdir -p dist
ditto build/macos/Folio.app dist/Folio.app
licenses=dist/Folio.app/Contents/Resources/licenses
mkdir -p "$licenses"
cp .deps/mupdf-1.28.6-source/COPYING "$licenses/MuPDF.txt"
cp .deps/mupdf-1.28.6-source/resources/fonts/noto/COPYING "$licenses/Noto.txt"
cp .deps/raylib-6.2/LICENSE "$licenses/raylib.txt"
codesign --force --sign - dist/Folio.app
printf '%s/dist/Folio.app\n' "$PWD"
