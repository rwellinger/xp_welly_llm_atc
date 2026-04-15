cmake_minimum_required(VERSION 3.21)
project(xp_wellys_atc CXX OBJCXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# ── Version from VERSION.txt ────────────────────────────────────────────────
file(READ "${CMAKE_SOURCE_DIR}/VERSION.txt" VERSION_STRING)
string(STRIP "${VERSION_STRING}" VERSION_STRING)

if(APPLE)
    if(NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
    endif()
    set(CMAKE_OSX_DEPLOYMENT_TARGET "12.0")
endif()

# ── X-Plane SDK headers ──────────────────────────────────────────────────────
include_directories(
    ${CMAKE_SOURCE_DIR}/sdk
    ${CMAKE_SOURCE_DIR}/sdk/XPLM
    ${CMAKE_SOURCE_DIR}/sdk/XPWidgets
    ${CMAKE_SOURCE_DIR}/vendor
)

# ── Dear ImGui ────────────────────────────────────────────────────────────────
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/vendor/imgui)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl2.cpp
)

# ── Plugin target ─────────────────────────────────────────────────────────────
add_library(xp_wellys_atc MODULE
    src/main.cpp
    src/atc_session.cpp
    src/atis_generator.cpp
    src/ptt_input.cpp
    src/audio_recorder.cpp
    src/whisper_client.cpp
    src/intent_parser.cpp
    src/atc_state_machine.cpp
    src/atc_templates.cpp
    src/airport_vrps.cpp
    src/airspace_db.cpp
    src/flight_phase.cpp
    src/gpt_client.cpp
    src/tts_client.cpp
    src/audio_player.cpp
    src/xplane_context.cpp
    src/settings.cpp
    src/atc_ui.cpp
    src/mic_permission.mm
    ${IMGUI_SOURCES}
)

target_include_directories(xp_wellys_atc PRIVATE
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

set_target_properties(xp_wellys_atc PROPERTIES
    OUTPUT_NAME "xp_wellys_atc"
    SUFFIX      ".xpl"
    PREFIX      ""
)

# ── Platform flags and libraries ──────────────────────────────────────────────
if(APPLE)
    target_compile_definitions(xp_wellys_atc PRIVATE
        APL=1 IBM=0 LIN=0
        XPLM200=1 XPLM210=1 XPLM300=1 XPLM301=1 XPLM400=1
        XP_WELLYS_ATC_VERSION="${VERSION_STRING}"
    )

    # Suppress OpenGL deprecation warnings in our code only (ImGui defines its own)
    set_source_files_properties(
        src/main.cpp src/atc_ui.cpp
        PROPERTIES COMPILE_DEFINITIONS "GL_SILENCE_DEPRECATION"
    )
    target_compile_options(xp_wellys_atc PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        -fvisibility=hidden
    )

    set(SDK_MAC_LIBS "${CMAKE_SOURCE_DIR}/sdk/Libraries/Mac")
    find_package(CURL REQUIRED)

    target_link_libraries(xp_wellys_atc PRIVATE
        "${SDK_MAC_LIBS}/XPLM.framework/Versions/Current/XPLM"
        "${SDK_MAC_LIBS}/XPWidgets.framework/Versions/Current/XPWidgets"
        "-framework OpenGL"
        "-framework AudioToolbox"
        "-framework AudioUnit"
        "-framework CoreAudio"
        "-framework AVFoundation"
        "-framework Security"
        "-framework CoreFoundation"
        CURL::libcurl
    )
endif()
