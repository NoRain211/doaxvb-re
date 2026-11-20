# SPDX-License-Identifier: GPL-3.0-or-later
# The macOS and Linux launcher, built on its own by tools/build_launcher.py so
# it can open before any game build: its Build page runs that build.
cmake_minimum_required(VERSION 3.20)
project(doaxbv_launcher CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(WIN32)
    message(FATAL_ERROR "Windows uses tools/launcher.ps1; this launcher is for macOS and Linux.")
endif()

include(../../recomp-runtime/cmake/RecompSDL3.cmake)
if(NOT SDL3_FOUND)
    message(FATAL_ERROR "The launcher needs SDL3; see docs/building.md.")
endif()

# stb_image decodes the PNG art setup copies from the player's disc (public domain).
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()
set(_stb_image "${CMAKE_BINARY_DIR}/_deps/stb/stb_image.h")
if(NOT EXISTS "${_stb_image}")
    file(DOWNLOAD
        https://raw.githubusercontent.com/nothings/stb/013ac3beddff3dbffafd5177e7972067cd2b5083/stb_image.h
        "${_stb_image}"
        EXPECTED_HASH SHA256=594c2fe35d49488b4382dbfaec8f98366defca819d916ac95becf3e75f4200b3)
endif()

add_executable(recomp_launcher ../launcher_sdl.cpp)
target_include_directories(recomp_launcher SYSTEM PRIVATE "${CMAKE_BINARY_DIR}/_deps/stb")
recomp_use_sdl3(recomp_launcher)
