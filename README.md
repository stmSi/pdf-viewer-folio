# Folio

A PDF viewer for macOS, Linux, Windows, Android, and WebAssembly.

## Install

- **macOS 11+** — Open `dist/Folio.app`. The app is built for the Mac's native architecture.
- **Linux x86-64** — Extract `dist/folio-linux-x86_64.tar.gz` and run `sh install.sh` inside. Run it again to upgrade. Use `sh install.sh --uninstall` to remove it. Requires glibc 2.35 and GLib 2.72 or newer.
- **Windows x86-64** — Extract `dist/folio-windows-x86_64.zip` and run `folio.exe`.
- **Android 10+** — Install `dist/folio-android-debug.apk` on ARM64 or x86-64.
- **Web** — Extract `dist/folio-web.zip`, run `python3 -m http.server --bind 127.0.0.1 8176` in that directory, and open [localhost:8176](http://127.0.0.1:8176/).

## Build

Use a Linux host with a C/C++ toolchain, CMake 3.22+, Ninja, GNU Make, Python 3, pkg-config, curl, file, tar, zip, and shasum (Perl's Digest::SHA). Native macOS and Android builds also support a macOS host.

For Linux builds on Ubuntu or Debian, run

```sh
sudo apt install libglib2.0-dev libgl-dev libx11-dev libxrandr-dev libxinerama-dev \
    libxcursor-dev libxi-dev libwayland-dev libxkbcommon-dev
```

| Target | Additional tools | Build and package |
| --- | --- | --- |
| macOS | Xcode Command Line Tools (`xcode-select --install`), CMake, Ninja, Python 3 | `./tools/build.sh macos` |
| Linux | Ubuntu 22.04 for the release compatibility baseline | `./tools/build.sh linux && ./tools/package-linux.sh` |
| Windows | x86-64 MinGW-w64 GCC and G++ | `./tools/build.sh windows && ./tools/package-zip.sh windows` |
| Android | JDK 17+, Android SDK API 35+, build-tools 36.0.0, NDK 29+. Set `ANDROID_HOME`. | `./tools/build.sh android` |
| Web | Emscripten SDK 4.0.23 activated | `./tools/build.sh web && ./tools/package-zip.sh web` |

MuPDF is available under the GNU AGPL or a commercial license.

### Reading themes

The **Theme** menu offers Original, Warm Paper, Dark, Sepia, Soft Gray, Sage, Dusk, and Night Amber. Themes tint both the PDF pages and the interface while retaining color in illustrations. The menu uses two columns in shorter windows so every theme stays accessible. Adjust **View → Page brightness** to dim the document further; the selected theme and brightness are remembered between launches.

### Reader features

- **Find** searches the document in the background, highlights matches, and moves between results. Search is case insensitive and can span text lines. Closing Find cancels the search.
- **Select and copy** by dragging across text with a mouse, or holding a word on Android and adjusting the selection handles. Use the floating Copy button or the system Copy shortcut. Selection stays within one page and respects PDF copy permissions.
- **Links and history** support internal page links and external web/email links. View → Back / Forward restores the previous reading position, zoom, layout, and rotation.
- **Reading → Pages** shows cached page thumbnails. Contents, Bookmarks, Recent, and Pages all scroll without opening items until you activate one.
- **View** offers continuous scrolling, a single page, two-page spreads, and rotation. Layout and rotation are saved with the reading position.
- **Native actions** include Finder PDF opening and menus on Mac, Android Open with / Share intake, and system Print, Share, and Save a Copy dialogs. Saving a copy preserves the original PDF bytes. Printing respects print permissions; Android prepares the selected pages as a vector PDF.
- **Background work** keeps native loading, page rendering, text extraction, and search off the interface thread. Opening and search show progress and can be cancelled. Page images, thumbnails, and text use viewport caches. The Web build uses cooperative work without native threads.
- **Accessibility** includes keyboard focus, labeled controls, links, and current-page text exposed to macOS accessibility and Android TalkBack. Text extraction depends on the PDF; scanned pages need an existing text layer. Native VoiceOver and TalkBack speech should also be checked on the intended device.

| Action | Keyboard |
| --- | --- |
| Open | Cmd/Ctrl+O |
| Find | Cmd/Ctrl+F |
| Next / previous result | F3 / Shift+F3; Mac Cmd+G / Cmd+Shift+G |
| Copy / select page text | Cmd/Ctrl+C / Cmd/Ctrl+A |
| Go to page | Cmd/Ctrl+G; Mac menu shortcut Cmd+L |
| Previous / next page or spread | Left / Right, Page Up / Page Down |
| Scroll | Up / Down, mouse wheel; drag or swipe on touch |
| Pan instead of selecting text | Space+drag or middle-mouse drag |
| Zoom | + / −; Cmd/Ctrl+wheel |
| Fit width / page | W / 0 |
| Back / forward | Alt+Left / Alt+Right; Mac Cmd+[ / Cmd+] |
| Move control focus / activate | Tab / Shift+Tab; Enter or Space |
| Print | Cmd/Ctrl+P |
| Save a copy on Mac | Cmd+Shift+S |

Annotations, form filling, OCR, and document editing are outside the current reader features.

### Reader checks

The optional native tests generate PDFs containing Unicode text, multiline search matches, links, mixed page rotations, encrypted documents, and a large page tree. They cover extraction, copying, search, print page selection, worker cancellation and cleanup, and reading-state compatibility.

```sh
cmake -S . -B build/macos -DFOLIO_BUILD_TESTS=ON
cmake --build build/macos --parallel 8
ctest --test-dir build/macos --output-on-failure
```

### macOS development

Run `./tools/build.sh macos && open dist/Folio.app` to build and open the native app. Use **Open PDF**, drag a PDF into the window, choose Folio in Finder's **Open With**, or run `open -a "$PWD/dist/Folio.app" /path/to/document.pdf`. Reading state is stored in `~/Library/Application Support/Folio`.

### Android development

Install the SDK, NDK, and an emulator system image through Android Studio's SDK Manager. The build selects the newest installed NDK; set `ANDROID_NDK_HOME` to use a specific one. On macOS:

```sh
export ANDROID_HOME="$HOME/Library/Android/sdk"
export PATH="$ANDROID_HOME/platform-tools:$ANDROID_HOME/emulator:$PATH"
./tools/build.sh android
emulator -list-avds
emulator -avd YOUR_AVD_NAME
```

With the emulator running (or an Android device connected with USB debugging enabled), install and launch:

```sh
adb install -r dist/folio-android-debug.apk
adb shell am start -n org.folio.viewer/.ViewerActivity
```

The default APK includes ARM64 and x86-64. For a faster build on an Apple Silicon emulator, use `ANDROID_ABIS=arm64-v8a ./tools/build.sh android`.

Open a PDF using **Open PDF** and the Android document picker. In **Reading → Contents**, drag or swipe the list, use a mouse wheel, or drag its scrollbar to browse without changing the selected entry or PDF page. Tap an entry to navigate; arrow keys move the keyboard selection and Enter opens it. Android Back closes menus, leaves the focused reading view, then exits the viewer.
