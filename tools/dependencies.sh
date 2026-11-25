#!/bin/sh
set -eu

cd "$(dirname "$0")/.."
mkdir -p .deps

fetch() {
    name=$1
    url=$2
    checksum=$3
    if [ -d ".deps/$name" ]; then
        return
    fi
    archive=".deps/$name.tar.gz"
    curl --fail --location --retry 2 --silent --show-error "$url" -o "$archive"
    printf '%s  %s\n' "$checksum" "$archive" | sha256sum -c
    tar -xzf "$archive" -C .deps
    rm "$archive"
}

fetch raylib-6.2 \
    https://codeload.github.com/raysan5/raylib/tar.gz/refs/tags/6.2 \
    df7a13129b1bec9e73a6962efff170beba310ee6eab8f94cd000b7316b4db5ee
fetch mupdf-1.28.6-source \
    https://mupdf.com/downloads/archive/mupdf-1.28.6-source.tar.gz \
    dc85000640a45d90ca9b77b061f6ca67cce0c4d4fbecf8819647baf573eef101
