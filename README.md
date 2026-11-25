# Folio

A PDF viewer for Linux, Windows, Android, and WebAssembly.

## Install

- **Linux x86-64** — Extract `dist/folio-linux-x86_64.tar.gz` and run `sh install.sh` inside. Run it again to upgrade. Use `sh install.sh --uninstall` to remove it. Requires glibc 2.35 and GLib 2.72 or newer.
- **Windows x86-64** — Extract `dist/folio-windows-x86_64.zip` and run `folio.exe`.
- **Android 10+** — Install `dist/folio-android-debug.apk` on ARM64 or x86-64.
- **Web** — Extract `dist/folio-web.zip`, run `python3 -m http.server --bind 127.0.0.1 8176` in that directory, and open [localhost:8176](http://127.0.0.1:8176/).

## Build

Use a Linux host with a C/C++ toolchain, CMake 3.22+, Ninja, GNU Make, Python 3, pkg-config, curl, file, tar, and zip.

For Linux builds on Ubuntu or Debian, run

```sh
sudo apt install libglib2.0-dev libgl-dev libx11-dev libxrandr-dev libxinerama-dev \
    libxcursor-dev libxi-dev libwayland-dev libxkbcommon-dev
```

| Target | Additional tools | Build and package |
| --- | --- | --- |
| Linux | Ubuntu 22.04 for the release compatibility baseline | `./tools/build.sh linux && ./tools/package-linux.sh` |
| Windows | x86-64 MinGW-w64 GCC and G++ | `./tools/build.sh windows && ./tools/package-zip.sh windows` |
| Android | JDK 17+, Android SDK API 35+, build-tools 36.0.0, NDK 30.0.16248370. Set `ANDROID_HOME`. | `./tools/build.sh android` |
| Web | Emscripten SDK 4.0.23 activated | `./tools/build.sh web && ./tools/package-zip.sh web` |

MuPDF is available under the GNU AGPL or a commercial license.
