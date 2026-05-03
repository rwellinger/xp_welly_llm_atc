cmake_minimum_required(VERSION 3.26)
project(xp_wellys_atc CXX OBJCXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_TESTS "Build Catch2 unit tests" ON)

# ── Sanitizer build (ASan + UBSan) ───────────────────────────────────────────
# When ON, instruments the SDK-free engine OBJECT lib + atc_repl + Catch2
# tests with AddressSanitizer + UndefinedBehaviorSanitizer. The plugin
# module (`xp_wellys_atc.xpl`) and vendored libs (whisper/llama/piper/
# ggml/imgui) are deliberately NOT instrumented — ASan inside the X-Plane
# process is fragile on macOS ARM64, and those vendor trees aren't built
# clean enough for sanitizers anyway.
#
# Use a separate build dir (e.g. `build-sanitize/`) so the normal
# `build/` Release tree stays untouched. Driven by `make sanitize`.
option(XP_WELLYS_ATC_SANITIZE "Enable ASan + UBSan on xp_atc_engine, atc_repl, and tests" OFF)
if(XP_WELLYS_ATC_SANITIZE)
    set(XP_WELLYS_ATC_SANITIZER_FLAGS
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
        -fno-sanitize-recover=all
    )
    message(STATUS "Sanitizer build: ASan + UBSan on xp_atc_engine, atc_repl, tests")
endif()

# ── ccache (optional) ────────────────────────────────────────────────────────
# Re-uses compiled object files across builds. Big win for the vendored
# llama.cpp / whisper.cpp / ggml / onnxruntime / espeak-ng / Piper trees,
# which change rarely between commits but are otherwise the long pole at
# 80%+ progress (single heavy TUs like t5-enc.cpp). No-op if ccache is not
# installed (brew install ccache on macOS, action handles it in CI).
find_program(CCACHE_PROGRAM ccache)
if(CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER      "${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER    "${CCACHE_PROGRAM}")
    set(CMAKE_OBJCXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS "ccache enabled: ${CCACHE_PROGRAM}")
endif()

# ── Version from VERSION.txt ────────────────────────────────────────────────
file(READ "${CMAKE_SOURCE_DIR}/VERSION.txt" VERSION_STRING)
string(STRIP "${VERSION_STRING}" VERSION_STRING)

# ARM64-only: whisper.cpp / llama.cpp Metal backends ship Apple Silicon
# Metal kernels, and Piper's onnxruntime build is ARM-targeted in the
# spike. An x86_64 build would not meet the < 3 s end-to-end target on
# Intel Macs (CPU-only inference), so we drop the universal-binary
# requirement here. Document this in README.md / RELEASE.md.
if(APPLE)
    if(NOT CMAKE_OSX_ARCHITECTURES)
        set(CMAKE_OSX_ARCHITECTURES "arm64")
    endif()
    # onnxruntime 1.22.0's prebuilt arm64 dylib is built against the
    # macOS 13.3 SDK; setting the deployment target lower triggers a
    # "newer version 13.3" linker warning and risks runtime crashes on
    # macOS 12. X-Plane 12 itself supports macOS 11+, but this plugin's
    # local-inference path requires macOS 13.3 due to onnxruntime —
    # document in README.
    set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3")
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

# libcurl is only needed by the plugin module (model downloader). The
# headless atc_repl tool no longer pulls it in.
find_package(CURL REQUIRED)

# ── Local inference backends (whisper.cpp, llama.cpp, Piper) ─────────────────
#
# We reuse the submodules that already live under spikes/spike_*/third_party/
# (initialised by `make setup` per the spike Makefiles) instead of pulling
# fresh copies under vendor/ — same git revisions, no extra disk + bandwidth,
# and the spikes stay buildable from the same source tree.
#
# Order matters: llama.cpp is added FIRST because both whisper.cpp and
# llama.cpp vendor `ggml` and short-circuit when the target already exists.
# Adding llama first locks in llama's pinned ggml; whisper then reuses that
# target instead of pulling in a second (potentially incompatible) copy.
# This dance is lifted from spike_e2e/CMakeLists.txt where it was validated.

set(SPIKES_DIR "${CMAKE_SOURCE_DIR}/spikes")

# --- shared ggml / metal options (apply to whichever subdir wins the ggml
# add_subdirectory race) ---
set(BUILD_SHARED_LIBS           OFF CACHE BOOL "" FORCE)
set(GGML_METAL                  ON  CACHE BOOL "" FORCE)
set(GGML_METAL_EMBED_LIBRARY    ON  CACHE BOOL "" FORCE)
set(GGML_ACCELERATE             ON  CACHE BOOL "" FORCE)
set(GGML_BLAS                   OFF CACHE BOOL "" FORCE)

# --- llama.cpp (provides `ggml`, `llama`, `common`) ---
set(LLAMA_BUILD_COMMON       ON  CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TOOLS        OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_EXAMPLES     OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_SERVER       OFF CACHE BOOL "" FORCE)
set(LLAMA_BUILD_TESTS        OFF CACHE BOOL "" FORCE)
set(LLAMA_CURL               OFF CACHE BOOL "" FORCE)
add_subdirectory(${SPIKES_DIR}/spike_llama/third_party/llama.cpp
                 ${CMAKE_BINARY_DIR}/llama.cpp EXCLUDE_FROM_ALL)

# --- whisper.cpp (provides `whisper`; reuses llama's ggml target) ---
set(WHISPER_BUILD_EXAMPLES      OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_TESTS         OFF CACHE BOOL "" FORCE)
set(WHISPER_BUILD_SERVER        OFF CACHE BOOL "" FORCE)
add_subdirectory(${SPIKES_DIR}/spike_whisper/third_party/whisper.cpp
                 ${CMAKE_BINARY_DIR}/whisper.cpp EXCLUDE_FROM_ALL)

# --- piper (provides `piper`; pulls in espeak-ng (static) + onnxruntime
# (prebuilt dylib)) ---
add_subdirectory(${SPIKES_DIR}/spike_piper/third_party/piper1-gpl/libpiper
                 ${CMAKE_BINARY_DIR}/libpiper EXCLUDE_FROM_ALL)

# espeak-ng-data is installed by libpiper's ExternalProject under the build
# dir. The plugin needs to find this dir at runtime; phase 4 (model loader)
# resolves it relative to the .xpl bundle, but until then we bake the build
# path so dev runs work.
set(XP_WELLYS_ATC_ESPEAKNG_DATA_DIR
    "${CMAKE_BINARY_DIR}/espeak_ng-install/share/espeak-ng-data"
    CACHE PATH "Path to espeak-ng-data used at runtime (dev fallback)")

# Path to onnxruntime prebuilt — we copy the dylib next to the .xpl so it
# resolves through @loader_path with no DYLD_LIBRARY_PATH gymnastics.
# libpiper's CMake extracts the prebuilt archive into its own source tree
# (CMAKE_CURRENT_LIST_DIR/lib), not the build dir.
set(ONNXRUNTIME_LIB_DIR
    "${SPIKES_DIR}/spike_piper/third_party/piper1-gpl/libpiper/lib/onnxruntime-osx-arm64-1.22.0/lib"
    CACHE PATH "Directory holding libonnxruntime.<ver>.dylib")

# ── Engine OBJECT library (X-Plane SDK-frei) ─────────────────────────────────
# Must compile and link without any XPLM / XPWidgets symbols.
# The intentional absence of -DXPLM200 etc. is a guardrail: any engine
# translation unit that pulls in <XPLM*.h> will fail to compile here.
#
# `backends/manager.cpp` lives here because it touches only std::thread
# + the abstract interfaces. The concrete WhisperStt / LlamaLm /
# PiperTts translation units live in the plugin module so the engine
# library does not pull whisper.h / llama.h / piper.h headers (and thus
# their static-lib link requirements) into the headless atc_repl tool.
add_library(xp_atc_engine OBJECT
    src/core/logging.cpp
    src/atc/engine.cpp
    src/atc/intent_parser.cpp
    src/atc/atc_state_machine.cpp
    src/atc/atc_templates.cpp
    src/atc/flight_phase.cpp
    src/atc/atis_generator.cpp
    src/atc/traffic_advisor.cpp
    src/atc/traffic_dialog.cpp
    src/backends/manager.cpp
    src/data/airport_vrps.cpp
    src/data/airspace_db.cpp
    src/data/traffic_context.cpp
    src/data/traffic_geometry.cpp
    src/core/xplane_context.cpp
    # Model manifest is SDK-free (data + SHA256 via CommonCrypto),
    # so it lives here and the headless atc_repl tool can use it
    # too if it ever needs to verify a local model.
    src/persistence/model_manifest.cpp
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
if(XP_WELLYS_ATC_SANITIZE)
    target_compile_options(xp_atc_engine PRIVATE
        ${XP_WELLYS_ATC_SANITIZER_FLAGS} -O1 -g
    )
endif()

# ── Plugin MODULE (Engine + X-Plane-spezifisch) ──────────────────────────────
add_library(xp_wellys_atc MODULE
    $<TARGET_OBJECTS:xp_atc_engine>
    src/main.cpp
    src/atc/atc_session.cpp
    src/core/xplane_context_runtime.cpp
    src/data/traffic_context_runtime.cpp
    src/audio/ptt_input.cpp
    src/audio/audio_recorder.cpp
    src/audio/audio_player.cpp
    src/audio/mic_permission.mm
    # Concrete inference backends — pull whisper.h / llama.h /
    # piper.h transitively. Static libs come from the spike
    # submodules added above.
    src/backends/whisper_stt.cpp
    src/backends/llama_lm.cpp
    src/backends/piper_tts.cpp
    # Loader orchestrates verification + concrete backend load on a
    # worker thread; uses XPLMGetPluginInfo (via model_paths) so it
    # has to live in the plugin module, not the engine library.
    src/backends/loader.cpp
    # Downloader: libcurl + std::filesystem on a worker thread.
    # Plugin-only because it depends on libcurl + plugin-relative
    # paths via model_paths.
    src/backends/downloader.cpp
    src/persistence/model_paths.cpp
    src/persistence/settings.cpp
    src/ui/atc_ui.cpp
    ${IMGUI_SOURCES}
)

target_include_directories(xp_wellys_atc PRIVATE
    ${CMAKE_SOURCE_DIR}/sdk
    ${CMAKE_SOURCE_DIR}/sdk/XPLM
    ${CMAKE_SOURCE_DIR}/sdk/XPWidgets
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/vendor
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
    # Local inference backends — headers from the spike submodules.
    ${SPIKES_DIR}/spike_whisper/third_party/whisper.cpp/include
    ${SPIKES_DIR}/spike_llama/third_party/llama.cpp/include
    ${SPIKES_DIR}/spike_llama/third_party/llama.cpp/ggml/include
    ${SPIKES_DIR}/spike_llama/third_party/llama.cpp/common
    ${SPIKES_DIR}/spike_piper/third_party/piper1-gpl/libpiper/include
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
        src/main.cpp src/ui/atc_ui.cpp
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
        "-framework CoreFoundation"
        # libcurl is now used solely for the in-plugin model
        # downloader (HTTPS GET with Range resume to HuggingFace);
        # OpenAI HTTP traffic is gone.
        CURL::libcurl
        # Local inference: whisper.cpp + llama.cpp build static and
        # link in directly. Piper is shared (libpiper.dylib) and is
        # resolved at runtime via @loader_path — see rpath setup
        # below.
        whisper
        common
        llama
        piper
    )

    # rpath: dev builds find dylibs in build/ and build/lib/<ort>; the
    # installed bundle finds them via @loader_path (Makefile install
    # target copies libpiper.dylib + libonnxruntime.<ver>.dylib next to
    # the .xpl).
    set_target_properties(xp_wellys_atc PROPERTIES
        BUILD_WITH_INSTALL_RPATH FALSE
        BUILD_RPATH "${CMAKE_BINARY_DIR};${ONNXRUNTIME_LIB_DIR}"
        INSTALL_RPATH "@loader_path"
    )

    # Stage the runtime dependencies next to the built .xpl so a fresh
    # `cmake --build` produces a directory the install step can copy
    # verbatim. We do this on a separate `stage_dylibs` target chained
    # to the plugin's POST_BUILD so it runs after `libpiper` and
    # `libonnxruntime` exist.
    add_custom_command(TARGET xp_wellys_atc POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "$<TARGET_FILE:piper>"
            "${CMAKE_BINARY_DIR}/$<TARGET_FILE_NAME:piper>"
        COMMAND ${CMAKE_COMMAND} -E echo
            "Staged libpiper next to xp_wellys_atc.xpl"
        COMMENT "Staging libpiper.dylib alongside xp_wellys_atc.xpl"
    )
    # Copy only the actual onnxruntime dylibs (not pkgconfig + dSYM).
    # The versioned + unversioned (symlink-style) names both need to be
    # present so libpiper's @rpath/libonnxruntime.1.22.0.dylib resolves.
    add_custom_command(TARGET xp_wellys_atc POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ONNXRUNTIME_LIB_DIR}/libonnxruntime.1.22.0.dylib"
            "${CMAKE_BINARY_DIR}/libonnxruntime.1.22.0.dylib"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${ONNXRUNTIME_LIB_DIR}/libonnxruntime.dylib"
            "${CMAKE_BINARY_DIR}/libonnxruntime.dylib"
        COMMENT "Staging libonnxruntime dylibs alongside xp_wellys_atc.xpl"
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
    tools/atc_repl/traffic_fixture.cpp
    tools/atc_repl/xplane_context_stub.cpp
)
target_include_directories(atc_repl PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/vendor
    ${CMAKE_SOURCE_DIR}/tools/atc_repl
)
# manager.cpp (in the engine OBJECT lib) calls curl_global_init/cleanup
# now, so the headless tool also needs libcurl at link time even
# though it never actually performs an HTTP transfer (no concrete
# backend ever registers in the CLI).
target_link_libraries(atc_repl PRIVATE CURL::libcurl)
if(APPLE)
    target_compile_options(atc_repl PRIVATE
        -Wall -Wextra -Wno-unused-parameter
    )
    set_target_properties(atc_repl PROPERTIES
        OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}"
    )
endif()
if(XP_WELLYS_ATC_SANITIZE)
    target_compile_options(atc_repl PRIVATE
        ${XP_WELLYS_ATC_SANITIZER_FLAGS} -O1 -g
    )
    target_link_options(atc_repl PRIVATE ${XP_WELLYS_ATC_SANITIZER_FLAGS})
endif()
set_target_properties(atc_repl PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)

# ── Tests ────────────────────────────────────────────────────────────────────
if(BUILD_TESTS)
    add_subdirectory(tests)
endif()
