cmake_minimum_required(VERSION 3.20)

project(doaxbv_recomp_runtime C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Official SDL3 VC build for the Windows input host; SDL3.dll is copied beside
# each target that uses it.
if(WIN32)
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif()
    include(FetchContent)
    FetchContent_Declare(SDL3
        URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-devel-3.4.16-VC.zip
        URL_HASH SHA256=1a784cb2a5c64d56fe7a62090fe9d242d9865f235e4ea9678f1a6ba4e693e7de)
    FetchContent_MakeAvailable(SDL3)
    find_package(SDL3 3.4 CONFIG REQUIRED PATHS "${sdl3_SOURCE_DIR}/cmake" NO_DEFAULT_PATH)
    function(recomp_use_sdl3 target)
        target_link_libraries(${target} PRIVATE SDL3::SDL3)
        add_custom_command(TARGET ${target} POST_BUILD COMMAND "${CMAKE_COMMAND}" -E
            copy_if_different "$<TARGET_FILE:SDL3::SDL3>" "$<TARGET_FILE_DIR:${target}>")
    endfunction()
endif()

# Optional SMAA from the third_party/smaa submodule; without it RECOMP_D3D_SMAA
# reports unavailable. The shader source is embedded as a byte list.
set(_smaa_dir "${CMAKE_CURRENT_SOURCE_DIR}/../third_party/smaa")
if(EXISTS "${_smaa_dir}/SMAA.hlsl")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${_smaa_dir}/SMAA.hlsl")
    file(READ "${_smaa_dir}/SMAA.hlsl" _smaa_hex HEX)
    string(REGEX REPLACE "(..)" "0x\\1," _smaa_hex "${_smaa_hex}")
    file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/smaa_hlsl.inc" CONTENT "${_smaa_hex}")
    # Only the presenter uses SMAA; scoping it keeps generated chunks untouched.
    set_source_files_properties(d3d_presenter_d3d11.cpp d3d_presenter_d3d11_test.cpp
        PROPERTIES COMPILE_DEFINITIONS RECOMP_SMAA
        INCLUDE_DIRECTORIES "${CMAKE_CURRENT_BINARY_DIR};${_smaa_dir}/Textures")
endif()

set(RECOMP_FUNCTION_SOURCE "" CACHE FILEPATH
    "Authenticated generated function source to compile")
set(RECOMP_FUNCTION_SHA256 "" CACHE STRING
    "SHA-256 from the generated function's authenticated receipt")
set(RECOMP_BOUNDARY_SOURCE "" CACHE FILEPATH
    "Optional authenticated generated caller/callee source to compile")
set(RECOMP_BOUNDARY_SHA256 "" CACHE STRING
    "SHA-256 of the authenticated generated caller/callee source")
set(RECOMP_ENTRY_SOURCE "" CACHE FILEPATH
    "Optional authenticated generated XBE entry source to compile")
set(RECOMP_ENTRY_SHA256 "" CACHE STRING
    "SHA-256 of the authenticated generated XBE entry source")
set(RECOMP_PROGRAM_DIR "" CACHE PATH
    "Authenticated complete generated-source snapshot")
set(RECOMP_PROGRAM_MANIFEST_SHA256 "" CACHE STRING
    "SHA-256 of the authenticated generated-source tree manifest")
if((RECOMP_FUNCTION_SOURCE AND NOT RECOMP_FUNCTION_SHA256) OR
   (RECOMP_FUNCTION_SHA256 AND NOT RECOMP_FUNCTION_SOURCE))
    message(FATAL_ERROR
        "RECOMP_FUNCTION_SOURCE and RECOMP_FUNCTION_SHA256 must be supplied together")
endif()
if((RECOMP_BOUNDARY_SOURCE AND NOT RECOMP_BOUNDARY_SHA256) OR
   (RECOMP_BOUNDARY_SHA256 AND NOT RECOMP_BOUNDARY_SOURCE))
    message(FATAL_ERROR
        "RECOMP_BOUNDARY_SOURCE and RECOMP_BOUNDARY_SHA256 must be supplied together")
endif()
if((RECOMP_ENTRY_SOURCE AND NOT RECOMP_ENTRY_SHA256) OR
   (RECOMP_ENTRY_SHA256 AND NOT RECOMP_ENTRY_SOURCE))
    message(FATAL_ERROR
        "RECOMP_ENTRY_SOURCE and RECOMP_ENTRY_SHA256 must be supplied together")
endif()
if((RECOMP_PROGRAM_DIR AND NOT RECOMP_PROGRAM_MANIFEST_SHA256) OR
   (RECOMP_PROGRAM_MANIFEST_SHA256 AND NOT RECOMP_PROGRAM_DIR))
    message(FATAL_ERROR
        "RECOMP_PROGRAM_DIR and RECOMP_PROGRAM_MANIFEST_SHA256 must be supplied together")
endif()

if(MSVC)
    set(_recomp_force_include "/FI${CMAKE_CURRENT_SOURCE_DIR}/runtime.h")
else()
    set(_recomp_force_include
        "-include" "${CMAKE_CURRENT_SOURCE_DIR}/runtime.h")
endif()

set(_recomp_test_leaf_source runtime_public_fixture.c)
set(_recomp_test_uses_public_fixture TRUE)
if(RECOMP_FUNCTION_SOURCE)
    get_filename_component(_recomp_function_source
        "${RECOMP_FUNCTION_SOURCE}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
    if(NOT EXISTS "${_recomp_function_source}")
        message(FATAL_ERROR "RECOMP_FUNCTION_SOURCE does not exist")
    endif()

    file(SHA256 "${_recomp_function_source}" _recomp_function_sha256)
    string(TOLOWER "${RECOMP_FUNCTION_SHA256}" _expected_function_sha256)
    if(NOT _recomp_function_sha256 STREQUAL _expected_function_sha256)
        message(FATAL_ERROR
            "RECOMP_FUNCTION_SOURCE does not match the authenticated receipt")
    endif()

    set_source_files_properties("${_recomp_function_source}" PROPERTIES
        COMPILE_DEFINITIONS RECOMP_GENERATED_CODE
        COMPILE_OPTIONS "${_recomp_force_include}")
    set(_recomp_test_leaf_source "${_recomp_function_source}")
    set(_recomp_test_uses_public_fixture FALSE)
endif()

set(RECOMP_PROGRAM_EBP_EXPECTED 11052 CACHE STRING
    "Expected generated EBP prologue override count")

add_executable(recomp_runtime_test
    runtime.c
    stop_report.c
    runtime_test.c
    animation_track_adapter_test.c
    xapi_time_adapter_test.cpp
    device_model.c
    device_model_test.c
    d3d_creation_model.c
    d3d_creation_adapter.c
    d3d_creation_model_test.c
    d3d_draw_model.c
    d3d_draw_adapter_test.c
    d3d_draw_model_test.c
    d3d_frame_model.c
    d3d_frame_model_test.c
    d3d_frame_adapter.c
    d3d_frame_adapter_test.c
    d3d_vblank.cpp
    d3d_presenter_memory.c
    d3d_presenter_memory_test.c
    d3d_render_state_model.c
    d3d_render_state_adapter.c
    d3d_render_state_model_test.c
    d3d_texture_model.c
    d3d_texture_adapter.c
    d3d_texture_model_test.c
    d3d_tile_model.c
    d3d_tile_adapter.c
    d3d_tile_model_test.c
    d3d_vertex_shader_model.c
    d3d_vertex_shader_adapter.c
    d3d_vertex_shader_model_test.c
    xbox_adpcm.c
    dsound_service_model.c
    dsound_service_adapter.c
    dsound_service_adapter_test.c
    custom_music.cpp
    soundtrack_adapter.c
    custom_music_test.cpp
    movie_color_model.c
    cri_service_model.c
    cri_service_model_test.c
    cri_service_adapter.c
    cri_service_adapter_test.c
    crt_format_model.c
    crt_format_adapter.c
    crt_format_adapter_test.c
    crt_string_adapter.c
    crt_string_adapter_test.c
    fiber_model.c
    fiber_adapter.c
    fiber_adapter_test.c
    fiber_model_test.c
    flag_macro_test.c
    sse_semantics_test.c
    directory_model.c
    directory_model_test.c
    input_model.c
    input_adapter.c
    input_model_test.c
    input_pulse_source.c
    input_pulse_source_test.c
    symbolic_link_model.c
    symbolic_link_model_test.c
    xapi_time_adapter.cpp
    program_manual.c
    animation_track.c
    animation_track_adapter.c
    animation_probe.c
    d3d_pose_replay.c
    animation_palette.c
    animation_skeleton.c
    animation_pose.c
    animation_seam.c
    animation_visual.c
    animation_camera.c
    animation_split_adapter.c
    d3d_split_pose.c
    save_adapter.c
    ohci_model.c
    ohci_model_test.c
    apu_model.c
    apu_model_test.c
    kernel_interrupt.c
    kernel_memory.c
    kernel_memory_test.c
    kernel_file.c
    kernel_file_save_test.c
    save_transaction.cpp
    kernel_rtl.c
    kernel_rtl_test.c
    kernel_crypto.c
    kernel_crypto_test.c
    kernel_thread.c
    kernel_thread_test.c
    kernel_video.c
    kernel_video_test.c
    kernel_ordinals.c
    "${_recomp_test_leaf_source}")
if(NOT MSVC)
    target_link_libraries(recomp_runtime_test PRIVATE m)
endif()
target_include_directories(recomp_runtime_test PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}")
# Media Foundation is delay-loaded: Windows N editions and Server without the
# Media Foundation feature lack it, and custom music then stays disabled.
set(RECOMP_MEDIA_FOUNDATION mfplat mfreadwrite mfuuid ole32 delayimp)
set(RECOMP_MEDIA_FOUNDATION_DELAYLOAD /DELAYLOAD:mfplat.dll /DELAYLOAD:mfreadwrite.dll)
target_link_libraries(recomp_runtime_test PRIVATE ${RECOMP_MEDIA_FOUNDATION})
target_link_options(recomp_runtime_test PRIVATE ${RECOMP_MEDIA_FOUNDATION_DELAYLOAD})
target_compile_definitions(recomp_runtime_test PRIVATE
    RECOMP_D3D_FRAME_ENABLED RECOMP_DSOUND_TEST_CLOCK)

if(_recomp_test_uses_public_fixture)
    target_compile_definitions(recomp_runtime_test PRIVATE
        RECOMP_PUBLIC_LEAF_FIXTURE)
endif()

if(RECOMP_BOUNDARY_SOURCE)
    get_filename_component(_recomp_boundary_source
        "${RECOMP_BOUNDARY_SOURCE}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
    if(NOT EXISTS "${_recomp_boundary_source}")
        message(FATAL_ERROR "RECOMP_BOUNDARY_SOURCE does not exist")
    endif()

    file(SHA256 "${_recomp_boundary_source}" _recomp_boundary_sha256)
    string(TOLOWER "${RECOMP_BOUNDARY_SHA256}" _expected_boundary_sha256)
    if(NOT _recomp_boundary_sha256 STREQUAL _expected_boundary_sha256)
        message(FATAL_ERROR
            "RECOMP_BOUNDARY_SOURCE does not match its authenticated SHA-256")
    endif()

    set_source_files_properties("${_recomp_boundary_source}" PROPERTIES
        COMPILE_DEFINITIONS RECOMP_GENERATED_CODE
        COMPILE_OPTIONS "${_recomp_force_include}")
    add_executable(recomp_runtime_boundary_test
        runtime.c
        stop_report.c
        runtime_boundary_test.c
        ohci_model.c
        kernel_ordinals.c
        apu_model.c
        "${_recomp_boundary_source}")
    target_include_directories(recomp_runtime_boundary_test PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}")
endif()

if(RECOMP_ENTRY_SOURCE)
    get_filename_component(_recomp_entry_source
        "${RECOMP_ENTRY_SOURCE}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
    if(NOT EXISTS "${_recomp_entry_source}")
        message(FATAL_ERROR "RECOMP_ENTRY_SOURCE does not exist")
    endif()

    file(SHA256 "${_recomp_entry_source}" _recomp_entry_sha256)
    string(TOLOWER "${RECOMP_ENTRY_SHA256}" _expected_entry_sha256)
    if(NOT _recomp_entry_sha256 STREQUAL _expected_entry_sha256)
        message(FATAL_ERROR
            "RECOMP_ENTRY_SOURCE does not match its authenticated SHA-256")
    endif()

    set_source_files_properties("${_recomp_entry_source}" PROPERTIES
        COMPILE_DEFINITIONS RECOMP_GENERATED_CODE
        COMPILE_OPTIONS "${_recomp_force_include}")
    add_executable(recomp_runner
        runtime.c
        stop_report.c
        runner.cpp
        runner_imports.c
        kernel_ordinals.c
        kernel_dispatch.c
        kernel_config.c
        kernel_debug.c
        device_model.c
        ohci_model.c
        apu_model.c
        kernel_device.c
        kernel_interrupt.c
        kernel_file.c
        save_transaction.cpp
        kernel_memory.c
        kernel_rtl.c
        kernel_crypto.c
        kernel_thread.c
        kernel_video.c
        host_diagnostics.cpp
        directory_model.c
        symbolic_link_model.c
        "${_recomp_entry_source}"
        ../xbe/sha.cpp
        ../xbe/xbe_parser.cpp)
    target_include_directories(recomp_runner PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../xbe")
    target_link_libraries(recomp_runner PRIVATE bcrypt dbghelp)
endif()

if(RECOMP_PROGRAM_DIR)
    get_filename_component(_recomp_program_dir
        "${RECOMP_PROGRAM_DIR}" ABSOLUTE BASE_DIR "${CMAKE_SOURCE_DIR}")
    file(GLOB _recomp_program_names LIST_DIRECTORIES false
        RELATIVE "${_recomp_program_dir}"
        "${_recomp_program_dir}/recomp_[0-9][0-9][0-9][0-9].c")
    list(SORT _recomp_program_names)
    if(NOT _recomp_program_names)
        message(FATAL_ERROR "Authenticated program has no source chunks")
    endif()
    set(_chunk_index 0)
    foreach(_name IN LISTS _recomp_program_names)
        string(REGEX REPLACE "^recomp_0*([0-9]+)\\.c$" "\\1" _number "${_name}")
        if(NOT _number EQUAL _chunk_index)
            message(FATAL_ERROR "Authenticated program chunk sequence has a gap: ${_name}")
        endif()
        math(EXPR _chunk_index "${_chunk_index} + 1")
    endforeach()
    # The manifest authenticates both membership and contents at any chunk count.
    # Never include the lifter's empty unresolved-stub file; use fail-loud stubs below.
    list(APPEND _recomp_program_names recomp_dispatch.c recomp_funcs.h)
    set(_recomp_program_sources)
    set(_recomp_program_manifest "")
    foreach(_name IN LISTS _recomp_program_names)
        set(_path "${_recomp_program_dir}/${_name}")
        if(NOT EXISTS "${_path}")
            message(FATAL_ERROR "Authenticated program member is missing: ${_name}")
        endif()
        file(SIZE "${_path}" _size)
        file(SHA256 "${_path}" _sha256)
        string(APPEND _recomp_program_manifest
            "${_name}\t${_size}\t${_sha256}\n")
        if(_name MATCHES "\\.c$")
            list(APPEND _recomp_program_sources "${_path}")
        endif()
    endforeach()
    string(SHA256 _recomp_program_manifest_sha256
        "${_recomp_program_manifest}")
    string(TOLOWER "${RECOMP_PROGRAM_MANIFEST_SHA256}"
        _expected_program_manifest_sha256)
    if(NOT _recomp_program_manifest_sha256 STREQUAL
       _expected_program_manifest_sha256)
        message(FATAL_ERROR
            "RECOMP_PROGRAM_DIR does not match its authenticated manifest")
    endif()

    # The lifter leaves the local EBP of every "push ebp; mov ebp, esp"
    # prologue undefined. The runtime holds the incoming guest EBP in
    # g_seh_ebp, which the generated fpo_leaf bodies already read.
    set(_ebp_before "\n    uint32_t ebp;\n")
    set(_ebp_after "\n    uint32_t ebp = g_seh_ebp;\n")
    set(_ebp_growth 12)
    if(NOT RECOMP_PROGRAM_EBP_EXPECTED MATCHES "^[0-9]+$")
        message(FATAL_ERROR
            "RECOMP_PROGRAM_EBP_EXPECTED must be a non-negative integer")
    endif()
    set(_ebp_expected "${RECOMP_PROGRAM_EBP_EXPECTED}")
    set(_ebp_applied 0)
    set(_recomp_derived_sources)
    foreach(_source IN LISTS _recomp_program_sources)
        file(READ "${_source}" _source_text)
        string(LENGTH "${_source_text}" _length_before)
        string(REPLACE "${_ebp_before}" "${_ebp_after}"
            _derived_text "${_source_text}")
        string(LENGTH "${_derived_text}" _length_after)
        math(EXPR _applied
            "(${_length_after} - ${_length_before}) / ${_ebp_growth}")
        math(EXPR _ebp_applied "${_ebp_applied} + ${_applied}")
        get_filename_component(_derived_name "${_source}" NAME_WE)
        set(_derived_source
            "${CMAKE_CURRENT_BINARY_DIR}/${_derived_name}_ebp.c")
        file(WRITE "${_derived_source}" "${_derived_text}")
        if(MSVC AND _derived_name STREQUAL "recomp_0016")
            # Retain the verified workaround for an MSVC Release optimizer hang.
            set_property(SOURCE "${_derived_source}" APPEND PROPERTY
                COMPILE_OPTIONS "$<$<CONFIG:Release>:/Od>")
        endif()
        list(APPEND _recomp_derived_sources "${_derived_source}")
    endforeach()
    if(NOT _ebp_applied EQUAL _ebp_expected)
        message(FATAL_ERROR
            "Authenticated EBP prologue count is ${_ebp_applied}, "
            "expected ${_ebp_expected}")
    endif()
    set(_recomp_program_sources ${_recomp_derived_sources})
    message(STATUS
        "Applied ${_ebp_applied} authenticated EBP prologue build overrides")

    set(_recomp_program_definitions)
    foreach(_source IN LISTS _recomp_program_sources)
        file(READ "${_source}" _source_text)
        string(REGEX MATCHALL
            "void[ \t\r\n]+sub_[0-9A-Fa-f]+[ \t\r\n]*\\([ \t\r\n]*void[ \t\r\n]*\\)"
            _matches "${_source_text}")
        foreach(_match IN LISTS _matches)
            string(REGEX MATCH "sub_[0-9A-Fa-f]+" _name "${_match}")
            list(APPEND _recomp_program_definitions "${_name}")
        endforeach()
    endforeach()
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/program_adapters.c"
        _adapter_text)
    string(REGEX MATCHALL
        "void[ \t\r\n]+sub_[0-9A-Fa-f]+[ \t\r\n]*\\([ \t\r\n]*void[ \t\r\n]*\\)"
        _adapter_matches "${_adapter_text}")
    foreach(_match IN LISTS _adapter_matches)
        string(REGEX MATCH "sub_[0-9A-Fa-f]+" _name "${_match}")
        list(APPEND _recomp_program_definitions "${_name}")
    endforeach()
    list(REMOVE_DUPLICATES _recomp_program_definitions)

    set(_recomp_program_references)
    foreach(_source IN LISTS _recomp_program_sources)
        file(READ "${_source}" _source_text)
        string(REGEX MATCHALL "sub_[0-9A-Fa-f]+[ \t\r\n]*\\("
            _references "${_source_text}")
        foreach(_reference IN LISTS _references)
            string(REGEX MATCH "sub_[0-9A-Fa-f]+" _name "${_reference}")
            list(APPEND _recomp_program_references "${_name}")
        endforeach()
    endforeach()
    list(REMOVE_DUPLICATES _recomp_program_references)
    set(_missing_source
        "${CMAKE_CURRENT_BINARY_DIR}/recomp_program_missing.c")
    file(WRITE "${_missing_source}"
        "#include <stdint.h>\nvoid recomp_program_missing(uint32_t);\n")
    set(_missing_count 0)
    foreach(_name IN LISTS _recomp_program_references)
        list(FIND _recomp_program_definitions "${_name}" _defined_index)
        if(_defined_index EQUAL -1)
            string(SUBSTRING "${_name}" 4 8 _address)
            file(APPEND "${_missing_source}"
                "void ${_name}(void) { recomp_program_missing(0x${_address}u); }\n")
            math(EXPR _missing_count "${_missing_count} + 1")
        endif()
    endforeach()
    message(STATUS
        "Generated ${_missing_count} fail-loud stubs for absent snapshot bodies")

    add_executable(recomp_program_runner
        runtime.c
        stop_report.c
        runner.cpp
        controller_settings.cpp
        program_adapters.c
        xapi_time_adapter.cpp
        program_manual.c
        animation_track.c
        animation_track_adapter.c
        animation_probe.c
        animation_palette.c
        animation_skeleton.c
        animation_pose.c
        animation_seam.c
        animation_visual.c
        animation_camera.c
        animation_split_adapter.c
        d3d_split_pose.c
        save_adapter.c
        d3d_creation_model.c
        d3d_creation_adapter.c
        d3d_draw_model.c
        d3d_draw_adapter.c
        d3d_frame_model.c
        d3d_frame_adapter.c
        d3d_vblank.cpp
        d3d_presenter_d3d11.cpp
        d3d_presenter_capture.cpp
        d3d_presenter_thread.cpp
        d3d_pose_replay.c
        d3d_vertex_program.cpp
        d3d_render_state_model.c
        d3d_render_state_adapter.c
        d3d_texture_model.c
        d3d_texture_adapter.c
        d3d_tile_model.c
        d3d_tile_adapter.c
        d3d_vertex_shader_model.c
        d3d_vertex_shader_adapter.c
        xbox_adpcm.c
        dsound_service_model.c
        dsound_service_adapter.c
        audio_output_xaudio2.cpp
        custom_music.cpp
        soundtrack_adapter.c
        movie_color_model.c
        cri_service_model.c
        cri_service_adapter.c
        crt_format_model.c
        crt_format_adapter.c
        crt_string_adapter.c
        fiber_model.c
        fiber_adapter.c
        directory_model.c
        input_model.c
        input_adapter.c
        input_host_win32.c
        input_pulse_source.c
        symbolic_link_model.c
        kernel_ordinals.c
        kernel_dispatch.c
        kernel_config.c
        kernel_debug.c
        device_model.c
        ohci_model.c
        apu_model.c
        kernel_device.c
        kernel_interrupt.c
        kernel_file.c
        save_transaction.cpp
        kernel_memory.c
        kernel_rtl.c
        kernel_crypto.c
        kernel_thread.c
        kernel_video.c
        host_diagnostics.cpp
        "${_missing_source}"
        ${_recomp_program_sources}
        ../xbe/sha.cpp
        ../xbe/xbe_parser.cpp)
    target_compile_definitions(recomp_program_runner PRIVATE
        RECOMP_FULL_PROGRAM
        RECOMP_D3D_FRAME_ENABLED)
    target_include_directories(recomp_program_runner PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}"
        "${_recomp_program_dir}"
        "${CMAKE_CURRENT_SOURCE_DIR}/../xbe")
    target_link_libraries(recomp_program_runner PRIVATE
        bcrypt dbghelp d3d11 d3dcompiler dwmapi dxgi user32 xaudio2 ${RECOMP_MEDIA_FOUNDATION})
    target_link_options(recomp_program_runner PRIVATE ${RECOMP_MEDIA_FOUNDATION_DELAYLOAD})
    if(NOT MSVC)
        target_link_libraries(recomp_program_runner PRIVATE m)
    endif()
    if(WIN32)
        recomp_use_sdl3(recomp_program_runner)
    endif()

    # The binary .ico stays out of git; see docs/building.md.
    set(RECOMP_APP_ICON "" CACHE FILEPATH "Optional .ico embedded as the runner icon")
    if(RECOMP_APP_ICON)
        file(TO_CMAKE_PATH "${RECOMP_APP_ICON}" app_icon)
        file(CONFIGURE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/app_icon.rc"
            CONTENT "1 ICON \"${app_icon}\"\n")
        target_sources(recomp_program_runner PRIVATE "${CMAKE_CURRENT_BINARY_DIR}/app_icon.rc")
    endif()
endif()

if(WIN32)
    add_executable(recomp_d3d_presenter_d3d11_test
        d3d_presenter_d3d11_test.cpp
        d3d_vblank.cpp
        d3d_vertex_program.cpp
        d3d_draw_model.c
        d3d_render_state_model.c
        d3d_texture_model.c)
    target_link_libraries(recomp_d3d_presenter_d3d11_test PRIVATE
        d3d11 d3dcompiler dxgi user32)
    add_executable(recomp_d3d_presenter_thread_test
        d3d_presenter_thread_test.cpp
        d3d_presenter_thread.cpp
        d3d_pose_replay.c
        d3d_split_pose.c
        animation_visual.c
        animation_camera.c
        animation_pose.c
        animation_skeleton.c
        animation_palette.c
        animation_seam.c
        d3d_presenter_capture.cpp
        d3d_presenter_d3d11.cpp
        d3d_vblank.cpp
        d3d_vertex_program.cpp
        d3d_draw_model.c
        d3d_render_state_model.c
        d3d_texture_model.c)
    target_link_libraries(recomp_d3d_presenter_thread_test PRIVATE
        d3d11 d3dcompiler dwmapi dxgi user32)
    add_executable(recomp_d3d_presenter_replay_test
        d3d_presenter_replay_test.cpp d3d_presenter_capture.cpp d3d_pose_replay.c
        d3d_split_pose.c animation_visual.c animation_camera.c animation_pose.c
        animation_skeleton.c animation_palette.c animation_seam.c d3d_draw_model.c)
    target_link_libraries(recomp_d3d_presenter_replay_test PRIVATE dwmapi user32)
    add_executable(recomp_animation_adapter_test animation_adapter_test.c
        animation_track.c d3d_split_pose.c animation_visual.c animation_camera.c
        animation_pose.c animation_skeleton.c animation_palette.c animation_seam.c
        d3d_pose_replay.c d3d_draw_model.c)
    if(NOT MSVC)
        target_link_libraries(recomp_animation_adapter_test PRIVATE m)
        target_link_libraries(recomp_d3d_presenter_replay_test PRIVATE m)
    endif()
    add_executable(recomp_audio_output_xaudio2_test audio_output_xaudio2_test.cpp)
    add_executable(recomp_input_host_test input_host_win32.c input_host_win32_test.c)
    target_link_libraries(recomp_input_host_test PRIVATE user32)
    recomp_use_sdl3(recomp_input_host_test)
endif()

add_executable(recomp_d3d_presenter_capture_test
    d3d_presenter_capture.cpp d3d_presenter_capture_test.cpp)

add_executable(recomp_save_transaction_test
    save_transaction.cpp save_transaction_test.cpp)

add_executable(recomp_xbox_adpcm_test xbox_adpcm.c xbox_adpcm_test.c)

add_executable(recomp_movie_color_test movie_color_model.c movie_color_model_test.c)

add_executable(recomp_animation_track_test animation_track.c animation_track_test.c)
if(NOT MSVC)
    target_link_libraries(recomp_animation_track_test PRIVATE m)
endif()

add_executable(recomp_animation_palette_test animation_palette.c animation_palette_test.c)
add_executable(recomp_animation_skeleton_test animation_skeleton_test.c)
add_executable(recomp_animation_pose_test animation_pose.c animation_skeleton.c animation_pose_test.c)
add_executable(recomp_d3d_pose_replay_test d3d_pose_replay.c d3d_pose_replay_test.c d3d_draw_model.c)
if(NOT MSVC)
    target_link_libraries(recomp_animation_skeleton_test PRIVATE m)
    target_link_libraries(recomp_animation_pose_test PRIVATE m)
    target_link_libraries(recomp_d3d_pose_replay_test PRIVATE m)
endif()

include(CTest)
add_test(NAME recomp-runtime-animation-track COMMAND recomp_animation_track_test)
add_test(NAME recomp-runtime-animation-palette COMMAND recomp_animation_palette_test)
add_test(NAME recomp-runtime-animation-skeleton COMMAND recomp_animation_skeleton_test)
add_test(NAME recomp-runtime-animation-pose COMMAND recomp_animation_pose_test)
add_test(NAME recomp-runtime-pose-replay COMMAND recomp_d3d_pose_replay_test)
add_test(NAME recomp-runtime-custom-music COMMAND recomp_runtime_test --custom-music)
set_tests_properties(recomp-runtime-custom-music PROPERTIES SKIP_RETURN_CODE 77)
add_test(NAME recomp-runtime-movie-color COMMAND recomp_movie_color_test)
add_test(NAME recomp-runtime-xbox-adpcm COMMAND recomp_xbox_adpcm_test)
if(TARGET recomp_audio_output_xaudio2_test)
    add_test(NAME recomp-runtime-audio-output COMMAND recomp_audio_output_xaudio2_test)
endif()
if(TARGET recomp_input_host_test)
    add_test(NAME recomp-runtime-input-host-sdl COMMAND recomp_input_host_test)
endif()
add_test(NAME recomp-runtime-save-transaction COMMAND recomp_save_transaction_test)
add_test(NAME recomp-runtime-d3d-presenter-capture COMMAND recomp_d3d_presenter_capture_test)
add_test(NAME recomp-runtime-four-cases COMMAND recomp_runtime_test)
foreach(draw_case IN ITEMS draw up)
    add_test(NAME recomp-runtime-presenter-close-${draw_case}
        COMMAND recomp_runtime_test --presenter-close-${draw_case})
    set_tests_properties(recomp-runtime-presenter-close-${draw_case} PROPERTIES
        PASS_REGULAR_EXPRESSION "host-window-close.*kind=normal-exit.*exit_code=0")
endforeach()
add_test(NAME recomp-runtime-presenter-close-release
    COMMAND recomp_runtime_test --presenter-close-release)
set_tests_properties(recomp-runtime-presenter-close-release PROPERTIES
    PASS_REGULAR_EXPRESSION "host-window-close.*kind=normal-exit.*exit_code=0")
add_test(
    NAME recomp-runtime-fiber-stack-recycling
    COMMAND recomp_runtime_test --fiber-stack-recycling)
set(_invalid_access_check
    "${CMAKE_CURRENT_BINARY_DIR}/check-invalid-access-$<CONFIG>.cmake")
file(GENERATE OUTPUT "${_invalid_access_check}" CONTENT [=[
execute_process(
    COMMAND "$<TARGET_FILE:recomp_runtime_test>" --invalid-access
    RESULT_VARIABLE result
    ERROR_VARIABLE error)
string(STRIP "${error}" error)
if(NOT result EQUAL 2)
    message(FATAL_ERROR "invalid access exited ${result}, expected 2")
endif()
if(NOT error MATCHES
        "^recomp runtime: undeclared 4-byte guest memory access at 0xffffffff")
    message(FATAL_ERROR "unexpected invalid-access diagnostic: ${error}")
endif()
foreach(mode IN ITEMS --ram-overrun --ram-pending-ohci)
    execute_process(
        COMMAND "$<TARGET_FILE:recomp_runtime_test>" "${mode}"
        RESULT_VARIABLE result ERROR_VARIABLE error)
    if(mode STREQUAL "--ram-overrun")
        set(expected "^recomp runtime: undeclared 4-byte guest memory access at 0x83ffffff")
    else()
        set(expected "^recomp runtime: unsupported OHCI write 0xffffffff at 0xfed00004")
    endif()
    if(NOT result EQUAL 2 OR NOT error MATCHES "${expected}")
        message(FATAL_ERROR "${mode}: exit=${result}, diagnostic=${error}")
    endif()
endforeach()
]=])
add_test(NAME recomp-runtime-invalid-access
    COMMAND "${CMAKE_COMMAND}" -P "${_invalid_access_check}")
if(TARGET recomp_runtime_boundary_test)
    add_test(NAME recomp-runtime-caller-callee-integration
        COMMAND recomp_runtime_boundary_test)
endif()
if(TARGET recomp_d3d_presenter_d3d11_test)
    add_test(NAME recomp-runtime-d3d11-texture-cache
        COMMAND recomp_d3d_presenter_d3d11_test)
    add_test(NAME recomp-runtime-d3d-presenter-replay COMMAND recomp_d3d_presenter_replay_test)
    foreach(mode IN ITEMS normal strict verify off)
        add_test(NAME recomp-runtime-animation-adapter-${mode} COMMAND recomp_animation_adapter_test ${mode})
    endforeach()
    add_test(NAME recomp-runtime-d3d-presenter-thread
        COMMAND recomp_d3d_presenter_thread_test)
endif()

if(MSVC)
    add_executable(recomp_stop_report_test
        stop_report_test.cpp stop_report.c host_diagnostics.cpp)
    target_link_libraries(recomp_stop_report_test PRIVATE dbghelp)
    set(_stop_report_check
        "${CMAKE_CURRENT_BINARY_DIR}/check-stop-report-$<CONFIG>.cmake")
    file(GENERATE OUTPUT "${_stop_report_check}" CONTENT [=[
foreach(mode IN ITEMS normal mismatch expected-error error boundary handled crash)
    set(log "${CMAKE_CURRENT_LIST_DIR}/stop-report-${mode}.log")
    file(REMOVE "${log}")
    execute_process(COMMAND "$<TARGET_FILE:recomp_stop_report_test>" "${mode}" "${log}"
        RESULT_VARIABLE result ERROR_VARIABLE error TIMEOUT 10)
    if(mode STREQUAL "crash")
        if(result STREQUAL "0" OR NOT error MATCHES "event=host-crash kind=unhandled-exception"
                OR error MATCHES "event=runtime-stop" OR EXISTS "${log}")
            message(FATAL_ERROR "${mode}: exit=${result}, diagnostic=${error}")
        endif()
        continue()
    endif()
    set(expected_code 0)
    set(fallback_code 0)
    set(kind "normal-exit")
    set(expectation "unchecked")
    if(mode STREQUAL "mismatch")
        set(expected_code 3)
        set(expectation "mismatch")
    elseif(mode STREQUAL "expected-error" OR mode STREQUAL "error")
        set(fallback_code 2)
        set(kind "runtime-error")
        if(mode STREQUAL "expected-error")
            set(expectation "match")
        else()
            set(expected_code 2)
        endif()
    elseif(mode STREQUAL "boundary")
        set(kind "diagnostic-boundary")
    endif()
    if(NOT result STREQUAL "${expected_code}" OR error MATCHES "event=host-crash")
        message(FATAL_ERROR "${mode}: exit=${result}, diagnostic=${error}")
    endif()
    file(READ "${log}" milestone)
    foreach(output IN ITEMS "${error}" "${milestone}")
        foreach(field IN ITEMS "event=runtime-stop" "kind=${kind}"
                "fallback_code=${fallback_code}" "exit_code=${expected_code}" "result=${expectation}")
            if(NOT output MATCHES "${field}")
                message(FATAL_ERROR "${mode}: missing ${field} in ${output}")
            endif()
        endforeach()
    endforeach()
    if(mode STREQUAL "handled" AND "$<CONFIG>" STREQUAL "Debug"
            AND NOT error MATCHES "event=exception-observed terminal=unknown")
        message(FATAL_ERROR "handled exception observation missing: ${error}")
    endif()
endforeach()
]=])
    add_test(NAME recomp-runtime-stop-report
        COMMAND "${CMAKE_COMMAND}" -P "${_stop_report_check}")
endif()

add_executable(recomp_controller_settings_test controller_settings.cpp controller_settings_test.cpp)
add_test(NAME recomp-runtime-controller-settings COMMAND recomp_controller_settings_test)

add_executable(recomp_animation_seam_test animation_seam.c animation_seam_test.c)
if(NOT MSVC)
    target_link_libraries(recomp_animation_seam_test PRIVATE m)
endif()
add_test(NAME recomp-runtime-animation-seam COMMAND recomp_animation_seam_test)

add_executable(recomp_d3d_split_pose_test d3d_split_pose_test.c d3d_split_pose.c
    d3d_pose_replay.c d3d_draw_model.c animation_visual.c animation_camera.c
    animation_pose.c animation_skeleton.c animation_palette.c animation_seam.c)
if(NOT MSVC)
    target_link_libraries(recomp_d3d_split_pose_test PRIVATE m)
endif()
add_test(NAME recomp-runtime-split-pose COMMAND recomp_d3d_split_pose_test)
