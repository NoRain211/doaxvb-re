# SPDX-License-Identifier: GPL-3.0-or-later
# SDL3 for macOS and Linux, shared by the runtime and the launcher
# (tools/launcher). macOS builds need SDL3 (brew install sdl3). Linux uses the
# system SDL3, or builds it when RECOMP_FETCH_SDL3 is on, as build_game.py asks (Ubuntu 24.04
# has no package). Hosts with none only configure, which the tools tests do
# without it.
option(RECOMP_FETCH_SDL3 "Build SDL3 when the system has none" OFF)
find_package(SDL3 QUIET)
if(APPLE AND NOT SDL3_FOUND)
    message(FATAL_ERROR "SDL3 is required: brew install sdl3")
endif()
set(_recomp_sdl3_beside FALSE)
if(NOT SDL3_FOUND AND RECOMP_FETCH_SDL3)
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif()
    include(FetchContent)
    # The release the Steam Deck build was tested with, pinned like the Windows zip.
    FetchContent_Declare(SDL3
        URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.12/SDL3-3.4.12.tar.gz
        URL_HASH SHA256=f07b958a9ac5020fb7a44cadb957f658b2149c3c8abb4f63145fac9303249db7)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_SHARED ON CACHE BOOL "" FORCE)
    set(SDL_STATIC OFF CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(SDL3)
    set(SDL3_FOUND TRUE)
    set(_recomp_sdl3_beside TRUE)
endif()

function(recomp_use_sdl3 target)
    if(NOT SDL3_FOUND)
        return()
    endif()
    target_link_libraries(${target} PRIVATE SDL3::SDL3)
    if(_recomp_sdl3_beside)
        # A private SDL3 travels with the program, so the game folder can move:
        # copy it beside the target and look there first.
        set_property(TARGET ${target} APPEND PROPERTY BUILD_RPATH "$ORIGIN")
        add_custom_command(TARGET ${target} POST_BUILD COMMAND "${CMAKE_COMMAND}" -E
            copy_if_different "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:${target}>/libSDL3.so.0")
    endif()
endfunction()
