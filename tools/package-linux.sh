#!/bin/sh
set -eu

cd "$(dirname "$0")/.."
binary=${1:-build/linux/folio}
if [ ! -f "$binary" ]; then
    printf 'Build the Linux executable first or supply its path.\n' >&2
    exit 1
fi
case "$(file -b "$binary")" in
    *'x86-64'*) arch=x86_64 ;;
    *'ARM aarch64'*) arch=aarch64 ;;
    *) printf 'Unknown Linux executable architecture: %s\n' "$binary" >&2; exit 1 ;;
esac
name="folio-linux-$arch"
mkdir -p dist
work=$(mktemp -d)
trap 'find "$work" -depth -delete' EXIT
mkdir -p "$work/$name/licenses"
install -m 755 "$binary" "$work/$name/folio"
install -m 755 platform/linux/install.sh "$work/$name/install.sh"
install -m 644 platform/linux/folio.png "$work/$name/folio.png"
cp .deps/mupdf-1.28.6-source/COPYING "$work/$name/licenses/MuPDF.txt"
cp .deps/mupdf-1.28.6-source/resources/fonts/noto/COPYING "$work/$name/licenses/Noto.txt"
cp .deps/raylib-6.2/LICENSE "$work/$name/licenses/raylib.txt"
printf '%s\n' "$arch" > "$work/$name/architecture"
tar --owner=0 --group=0 --numeric-owner -czf "dist/$name.tar.gz" -C "$work" "$name"
printf '%s\n' "dist/$name.tar.gz"
