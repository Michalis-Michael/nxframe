# NxFrame - broadcast contribution encoder/decoder
#
# Copyright (C) 2026 Michalis Michael
#
# SPDX-License-Identifier: GPL-3.0-or-later
#
# Public root build file.
#
# This CMake file is intentionally repository-oriented:
# - no machine-local absolute paths
# - dependencies are discovered through pkg-config / cache variables
# - the DeckLink SDK location is configurable
# - app and test builds can be enabled independently
# - AVX2 code is isolated in one translation unit with scalar fallback available

cmake_minimum_required(VERSION 3.16)

project(NxFrame
    VERSION 0.1.0
    DESCRIPTION "Low-latency broadcast contribution encoder/decoder"
    LANGUAGES CXX
)

include(GNUInstallDirs)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# ---------------- Build options ----------------
option(NXFRAME_BUILD_APP "Build the NxFrame sender/receiver application" ON)
option(NXFRAME_BUILD_TESTS "Build NxFrame validation tests" ON)
option(NXFRAME_BUILD_PURE_TESTS "Build tests that do not require FFmpeg/SRT/DeckLink" ON)
option(NXFRAME_BUILD_FFMPEG_TESTS "Build tests that require FFmpeg libraries" ON)
option(NXFRAME_ENABLE_AVX2 "Build the AVX2 v210 converter translation unit" ON)
option(NXFRAME_ENABLE_RELEASE_TUNING "Apply NxFrame release compile tuning on Release builds" ON)
option(NXFRAME_SANITIZE "Enable AddressSanitizer/UndefinedBehaviorSanitizer on supported compilers" OFF)
option(NXFRAME_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(NXFRAME_INSTALL "Enable install rules for the NxFrame application" ON)

set(NXFRAME_DECKLINK_SDK_DIR "" CACHE PATH
    "Path to the external Blackmagic DeckLink SDK Linux include directory containing DeckLinkAPI.h and DeckLinkAPIDispatch.cpp"
)

find_package(Threads REQUIRED)

# ---------------- Common target policy ----------------
function(nxframe_apply_common_options target_name)
    target_compile_features(${target_name} PRIVATE cxx_std_14)

    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target_name} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
        )

        if(NXFRAME_WARNINGS_AS_ERRORS)
            target_compile_options(${target_name} PRIVATE -Werror)
        endif()

        if(NXFRAME_ENABLE_RELEASE_TUNING AND NOT NXFRAME_SANITIZE)
            target_compile_options(${target_name} PRIVATE
                $<$<CONFIG:Release>:-O3>
            )
        endif()

        if(NXFRAME_SANITIZE)
            target_compile_options(${target_name} PRIVATE
                -O1
                -g
                -fno-omit-frame-pointer
                -fsanitize=address,undefined
            )
            target_link_options(${target_name} PRIVATE
                -fsanitize=address,undefined
            )
        endif()
    endif()
endfunction()

function(nxframe_configure_avx2_source)
    if(NXFRAME_ENABLE_AVX2 AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        set_source_files_properties(
            input/simd_v210_avx2.cpp
            PROPERTIES COMPILE_OPTIONS "-mavx2;-mfma"
        )
    endif()
endfunction()

function(nxframe_find_decklink_sdk out_dir)
    set(_candidates)

    if(NXFRAME_DECKLINK_SDK_DIR)
        list(APPEND _candidates "${NXFRAME_DECKLINK_SDK_DIR}")
    endif()

    if(DEFINED ENV{DECKLINK_SDK_DIR})
        list(APPEND _candidates "$ENV{DECKLINK_SDK_DIR}")
    endif()

    # The public repository does not bundle the Blackmagic DeckLink SDK.
    # Pass NXFRAME_DECKLINK_SDK_DIR, set DECKLINK_SDK_DIR, or install the SDK
    # headers/dispatch source into a system include location.
    list(APPEND _candidates
        "/usr/local/include/DeckLinkAPI"
        "/usr/local/include/decklink"
        "/usr/include/DeckLinkAPI"
        "/usr/include/decklink"
    )

    foreach(_candidate IN LISTS _candidates)
        if(EXISTS "${_candidate}/DeckLinkAPI.h" AND EXISTS "${_candidate}/DeckLinkAPIDispatch.cpp")
            set(${out_dir} "${_candidate}" PARENT_SCOPE)
            return()
        endif()
    endforeach()

    message(FATAL_ERROR
        "Blackmagic DeckLink SDK was not found.\n"
        "Set -DNXFRAME_DECKLINK_SDK_DIR=/path/to/DeckLinkSDK/Linux/include "
        "or set the DECKLINK_SDK_DIR environment variable. The directory must contain "
        "DeckLinkAPI.h and DeckLinkAPIDispatch.cpp."
    )
endfunction()

function(nxframe_find_public_dependencies)
    find_package(PkgConfig REQUIRED)

    pkg_check_modules(AVUTIL REQUIRED IMPORTED_TARGET libavutil)
    pkg_check_modules(AVCODEC REQUIRED IMPORTED_TARGET libavcodec)
    pkg_check_modules(AVFORMAT REQUIRED IMPORTED_TARGET libavformat)
    pkg_check_modules(AVDEVICE REQUIRED IMPORTED_TARGET libavdevice)
    pkg_check_modules(SWSCALE REQUIRED IMPORTED_TARGET libswscale)
    pkg_check_modules(SWRESAMPLE REQUIRED IMPORTED_TARGET libswresample)
endfunction()

# ---------------- Pure tests ----------------
if(NXFRAME_BUILD_TESTS)
    enable_testing()

    if(NXFRAME_BUILD_PURE_TESTS)
        add_executable(test_v210_scalar_vs_avx2
            tests/test_v210_scalar_vs_avx2.cpp
            input/simd_v210_scalar.cpp
            input/simd_v210_avx2.cpp
        )
        target_include_directories(test_v210_scalar_vs_avx2 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        nxframe_configure_avx2_source()
        target_link_libraries(test_v210_scalar_vs_avx2 PRIVATE Threads::Threads)
        nxframe_apply_common_options(test_v210_scalar_vs_avx2)
        add_test(NAME v210_scalar_vs_avx2 COMMAND test_v210_scalar_vs_avx2)

        add_executable(test_signal_generator_layout
            tests/test_signal_generator_layout.cpp
            input/test_signal_generator.cpp
        )
        target_include_directories(test_signal_generator_layout PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_link_libraries(test_signal_generator_layout PRIVATE m)
        nxframe_apply_common_options(test_signal_generator_layout)
        add_test(NAME signal_generator_layout COMMAND test_signal_generator_layout)

        add_executable(test_bounded_queue_policy
            tests/test_bounded_queue_policy.cpp
        )
        target_include_directories(test_bounded_queue_policy PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
        target_link_libraries(test_bounded_queue_policy PRIVATE Threads::Threads)
        nxframe_apply_common_options(test_bounded_queue_policy)
        add_test(NAME bounded_queue_policy COMMAND test_bounded_queue_policy)
    endif()
endif()

# ---------------- Shared FFmpeg dependency discovery ----------------
if(NXFRAME_BUILD_APP OR (NXFRAME_BUILD_TESTS AND NXFRAME_BUILD_FFMPEG_TESTS))
    nxframe_find_public_dependencies()
endif()

# ---------------- Application ----------------
if(NXFRAME_BUILD_APP)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SRT REQUIRED IMPORTED_TARGET srt)

    nxframe_find_decklink_sdk(DECKLINK_SDK_DIR)

    add_library(DeckLinkAPI STATIC
        "${DECKLINK_SDK_DIR}/DeckLinkAPIDispatch.cpp"
    )
    target_include_directories(DeckLinkAPI SYSTEM PUBLIC "${DECKLINK_SDK_DIR}")
    nxframe_apply_common_options(DeckLinkAPI)

    set(NXFRAME_APP_SOURCES
        main.cpp

        app/nxframe_app.cpp
        app/play_app.cpp
        app/send_app.cpp

        cli/cli_utils.cpp
        cli/receiver_cli.cpp
        cli/transport_url.cpp

        config/preset_validator.cpp

        sender/sender_pipeline.cpp
        sender/video_encode_worker.cpp
        sender/audio_encode_worker.cpp

        input/decklink.cpp
        input/simd_v210_scalar.cpp
        input/simd_v210_avx2.cpp
        input/test_signal_generator.cpp
        input/input_manager.cpp

        receiver/srt_input.cpp
        receiver/udp_input.cpp
        receiver/receiver.cpp
        receiver/demuxer_ts.cpp
        receiver/decoder_video.cpp
        receiver/decoder_audio.cpp
        playout/av_sync_controller.cpp

        encoders/encoder_manager.cpp
        encoders/encoder_x264.cpp
        encoders/encoder_x265.cpp
        encoders/encoder_aac.cpp
        encoders/encoder_pcm.cpp

        output/srt.cpp
        output/udp_streamer.cpp
        output/muxer_ts.cpp
        output/output_manager.cpp
        output/decklink_output.cpp
    )

    set(NXFRAME_PUBLIC_HEADERS
        core/frame.h
        core/bounded_queue.h
        core/packet_item.h
        core/stop_token.h
        core/pipeline_telemetry.h
        stage_timing.h
    )

    add_executable(NxFrame
        ${NXFRAME_APP_SOURCES}
        ${NXFRAME_PUBLIC_HEADERS}
    )

    target_include_directories(NxFrame PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
    )

    nxframe_configure_avx2_source()
    nxframe_apply_common_options(NxFrame)

    target_link_libraries(NxFrame PRIVATE
        DeckLinkAPI
        PkgConfig::SRT
        PkgConfig::AVUTIL
        PkgConfig::AVCODEC
        PkgConfig::AVFORMAT
        PkgConfig::AVDEVICE
        PkgConfig::SWSCALE
        PkgConfig::SWRESAMPLE
        Threads::Threads
        ${CMAKE_DL_LIBS}
    )

    if(NXFRAME_INSTALL)
        install(TARGETS NxFrame RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
        install(DIRECTORY preset/ DESTINATION ${CMAKE_INSTALL_DATADIR}/nxframe/preset)
        install(DIRECTORY docs/ DESTINATION ${CMAKE_INSTALL_DATADIR}/nxframe/docs OPTIONAL)
    endif()
endif()

# ---------------- FFmpeg-backed tests ----------------
if(NXFRAME_BUILD_TESTS AND NXFRAME_BUILD_FFMPEG_TESTS)
    add_executable(test_av_sync_controller_lock
        tests/test_av_sync_controller_lock.cpp
        playout/av_sync_controller.cpp
    )
    target_include_directories(test_av_sync_controller_lock PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_av_sync_controller_lock PRIVATE
        PkgConfig::AVUTIL
        Threads::Threads
    )
    nxframe_apply_common_options(test_av_sync_controller_lock)
    add_test(NAME av_sync_controller_lock COMMAND test_av_sync_controller_lock)

    add_executable(test_demuxer_ts_health
        tests/test_demuxer_ts_health.cpp
        receiver/demuxer_ts.cpp
    )
    target_include_directories(test_demuxer_ts_health PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_demuxer_ts_health PRIVATE
        PkgConfig::AVUTIL
        PkgConfig::AVCODEC
        PkgConfig::AVFORMAT
        Threads::Threads
    )
    nxframe_apply_common_options(test_demuxer_ts_health)
    add_test(NAME demuxer_ts_health COMMAND test_demuxer_ts_health)

    add_executable(test_muxer_session_anchor_state
        tests/test_muxer_session_anchor_state.cpp
        output/muxer_ts.cpp
    )
    target_include_directories(test_muxer_session_anchor_state PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(test_muxer_session_anchor_state PRIVATE
        PkgConfig::AVUTIL
        PkgConfig::AVCODEC
        PkgConfig::AVFORMAT
        Threads::Threads
    )
    nxframe_apply_common_options(test_muxer_session_anchor_state)
    add_test(NAME muxer_session_anchor_state COMMAND test_muxer_session_anchor_state)
endif()
