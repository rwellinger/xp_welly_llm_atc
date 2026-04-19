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

# ── Dear ImGui ────────────────────────────────────────────────────────────────
set(IMGUI_DIR ${CMAKE_SOURCE_DIR}/vendor/imgui)
set(IMGUI_SOURCES
    ${IMGUI_DIR}/imgui.cpp
    ${IMGUI_DIR}/imgui_draw.cpp
    ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp
    ${IMGUI_DIR}/backends/imgui_impl_opengl2.cpp
)

find_package(CURL REQUIRED)

# ── Engine OBJECT library (X-Plane SDK-frei) ─────────────────────────────────
# Must compile and link without any XPLM / XPWidgets symbols.
# The intentional absence of -DXPLM200 etc. is a guardrail: any engine
# translation unit that pulls in <XPLM*.h> will fail to compile here.
add_library(xp_atc_engine OBJECT
    src/logging.cpp
    src/engine/engine.cpp
    src/intent_parser.cpp
    src/atc_state_machine.cpp
    src/atc_templates.cpp
    src/flight_phase.cpp
    src/atis_generator.cpp
    src/gpt_client.cpp
    src/whisper_client.cpp
    src/tts_client.cpp
    src/airport_vrps.cpp
    src/airspace_db.cpp
    src/xplane_context.cpp
)
target_include_directories(xp_atc_engine PRIVATE
    ${CMAKE_SOURCE_DIR}/vendor
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_definitions(xp_atc_engine PRIVATE
    XP_WELLYS_ATC_VERSION="${VERSION_STRING}"
)
if(APPLE)
    target_compile_options(xp_atc_engine PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        -fvisibility=hidden
    )
endif()
target_link_libraries(xp_atc_engine PRIVATE CURL::libcurl)

# ── Plugin MODULE (Engine + X-Plane-spezifisch) ──────────────────────────────
add_library(xp_wellys_atc MODULE
    $<TARGET_OBJECTS:xp_atc_engine>
    src/main.cpp
    src/atc_session.cpp
    src/xplane_context_runtime.cpp
    src/ptt_input.cpp
    src/audio_recorder.cpp
    src/audio_player.cpp
    src/settings.cpp
    src/atc_ui.cpp
    src/mic_permission.mm
    ${IMGUI_SOURCES}
)

target_include_directories(xp_wellys_atc PRIVATE
    ${CMAKE_SOURCE_DIR}/sdk
    ${CMAKE_SOURCE_DIR}/sdk/XPLM
    ${CMAKE_SOURCE_DIR}/sdk/XPWidgets
    ${CMAKE_SOURCE_DIR}/vendor
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

set_target_properties(xp_wellys_atc PROPERTIES
    OUTPUT_NAME "xp_wellys_atc"
    SUFFIX      ".xpl"
    PREFIX      ""
)

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

# ── CLI Executable (ohne X-Plane) ────────────────────────────────────────────
# Host-arch only — no need for universal binary on a dev tool.
add_executable(atc_repl
    $<TARGET_OBJECTS:xp_atc_engine>
    tools/atc_repl/main.cpp
    tools/atc_repl/scenario.cpp
    tools/atc_repl/repl.cpp
    tools/atc_repl/settings_stub.cpp
    tools/atc_repl/xplane_context_stub.cpp
)
target_include_directories(atc_repl PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/vendor
)
target_link_libraries(atc_repl PRIVATE CURL::libcurl)
if(APPLE)
    target_compile_options(atc_repl PRIVATE
        -Wall -Wextra -Wno-unused-parameter
    )
    set_target_properties(atc_repl PROPERTIES
        OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}"
    )
endif()
set_target_properties(atc_repl PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
