cmake_minimum_required(VERSION 3.22)
project(Folio VERSION 0.1.0 LANGUAGES C)

set(RAYLIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.deps/raylib-6.2")
set(MUPDF_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.deps/mupdf-1.28.6-source")
set(MUPDF_BUILD_DIR "" CACHE PATH "Directory containing the target MuPDF static libraries")
if(NOT EXISTS "${RAYLIB_DIR}/CMakeLists.txt" OR
   NOT EXISTS "${MUPDF_BUILD_DIR}/libmupdf.a" OR
   NOT EXISTS "${MUPDF_BUILD_DIR}/libmupdf-third.a")
    message(FATAL_ERROR "Use tools/build.sh to fetch and build the dependencies for this target.")
endif()

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(CUSTOMIZE_BUILD ON CACHE BOOL "" FORCE)
set(USE_AUDIO OFF CACHE BOOL "" FORCE)
set(SUPPORT_MODULE_RAUDIO OFF CACHE BOOL "" FORCE)
set(SUPPORT_MODULE_RMODELS OFF CACHE BOOL "" FORCE)
set(USE_EXTERNAL_GLFW OFF CACHE STRING "" FORCE)
if(EMSCRIPTEN)
    set(PLATFORM Web CACHE STRING "" FORCE)
elseif(ANDROID)
    set(PLATFORM Android CACHE STRING "" FORCE)
else()
    set(PLATFORM Desktop CACHE STRING "" FORCE)
    if(UNIX)
        set(GLFW_BUILD_X11 ON CACHE BOOL "" FORCE)
        set(GLFW_BUILD_WAYLAND ON CACHE BOOL "" FORCE)
    endif()
endif()
add_subdirectory("${RAYLIB_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/raylib" EXCLUDE_FROM_ALL)

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(FONT_FILE "${MUPDF_DIR}/resources/fonts/noto/NotoSans-Regular.otf")
set(EMBEDDED_FONT "${CMAKE_CURRENT_BINARY_DIR}/embedded_font.c")
add_custom_command(
    OUTPUT "${EMBEDDED_FONT}"
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed-font.py"
            "${FONT_FILE}" "${EMBEDDED_FONT}"
    DEPENDS "${FONT_FILE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed-font.py"
    VERBATIM)

set(FOLIO_SOURCES src/main.c src/pdf.c src/platform.c src/reading_state.c "${EMBEDDED_FONT}")
if(ANDROID)
    add_library(folio SHARED ${FOLIO_SOURCES})
else()
    add_executable(folio ${FOLIO_SOURCES})
endif()

target_include_directories(folio SYSTEM PRIVATE "${MUPDF_DIR}/include")
target_compile_features(folio PRIVATE c_std_11)
target_link_libraries(folio PRIVATE raylib
    "${MUPDF_BUILD_DIR}/libmupdf.a" "${MUPDF_BUILD_DIR}/libmupdf-third.a")
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(folio PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(EMSCRIPTEN)
    set_target_properties(folio PROPERTIES OUTPUT_NAME index SUFFIX ".html")
    set(WEB_SHELL "${CMAKE_CURRENT_SOURCE_DIR}/platform/web/shell.html")
    set(WEB_RUNTIME "${CMAKE_CURRENT_SOURCE_DIR}/platform/web/runtime.js")
    set_property(TARGET folio APPEND PROPERTY LINK_DEPENDS "${WEB_SHELL}" "${WEB_RUNTIME}")
    target_compile_options(folio PRIVATE -fwasm-exceptions)
    target_link_options(folio PRIVATE
        -fwasm-exceptions
        -sSUPPORT_LONGJMP=wasm
        -sALLOW_MEMORY_GROWTH=1
        -sINITIAL_MEMORY=67108864
        -sENVIRONMENT=web
        "SHELL:--shell-file \"${WEB_SHELL}\""
        "SHELL:--post-js \"${WEB_RUNTIME}\"")
elseif(ANDROID)
    target_include_directories(folio SYSTEM PRIVATE "${CMAKE_ANDROID_NDK}/sources/android/native_app_glue")
    target_link_options(folio PRIVATE -Wl,-u,ANativeActivity_onCreate -Wl,-z,max-page-size=16384)
elseif(WIN32)
    target_link_libraries(folio PRIVATE comdlg32 shell32)
    if(MINGW)
        target_link_options(folio PRIVATE -static -mwindows)
    endif()
else()
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(GIO REQUIRED IMPORTED_TARGET gio-2.0)
    target_link_libraries(folio PRIVATE PkgConfig::GIO)
endif()
