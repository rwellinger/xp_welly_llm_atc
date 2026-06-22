cmake_minimum_required(VERSION 3.26)
# Objective-C++ is only available on macOS (for .mm sources).
# Detect before project() to avoid requesting it on Linux.
if(APPLE)
    project(xp_wellys_atc CXX OBJCXX)
else()
    project(xp_wellys_atc CXX)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(BUILD_TESTS "Build Catch2 unit tests" ON)

# ── Local-inference toggle ───────────────────────────────────────────────────
# When ON, the plugin module is compiled with whisper.cpp + llama.cpp +
# Piper (Metal-accelerated, Apple Silicon only). When OFF, those
# backends are skipped entirely and only the OpenAI cloud clients are
# built — letting the same source tree produce an x86_64 (Intel)
# slice that runs against api.openai.com.
#
# A universal binary is assembled by building twice (once per
# architecture) and lipo-ing the two .xpl outputs together — see
# `make build`.
option(XPWELLYS_USE_LOCAL_INFERENCE
    "Compile local whisper.cpp/llama.cpp/Piper backends (Apple Silicon only)"
    ON)

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

# Architecture + deployment target.
#
# Local inference (whisper.cpp / llama.cpp / Piper) ships Apple Silicon
# Metal kernels and uses an arm64-only onnxruntime prebuilt, so any
# build that enables XPWELLYS_USE_LOCAL_INFERENCE must target arm64.
# The OpenAI-only build is libcurl + system frameworks all the way
# down and is architecture-neutral, so it cleanly produces an x86_64
# slice for Intel Macs. The two slices are then lipo'd together into
# one universal .xpl by `make build`.
if(APPLE)
    if(XPWELLYS_USE_LOCAL_INFERENCE)
        if(NOT CMAKE_OSX_ARCHITECTURES)
            set(CMAKE_OSX_ARCHITECTURES "arm64")
        endif()
        # onnxruntime 1.22.0's prebuilt arm64 dylib is built against
        # the macOS 13.3 SDK; setting the deployment target lower
        # triggers a "newer version 13.3" linker warning and risks
        # runtime crashes on macOS 12.
        set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3")
    else()
        # OpenAI-only build: caller picks the architecture (typically
        # x86_64 for the universal-binary's Intel slice). Keep the
        # 13.3 deployment target so both slices match and lipo is
        # happy — bumping it lower would split the merged binary's
        # LC_BUILD_VERSION load commands.
        if(NOT CMAKE_OSX_ARCHITECTURES)
            set(CMAKE_OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}")
        endif()
        set(CMAKE_OSX_DEPLOYMENT_TARGET "13.3")
    endif()
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

if(XPWELLYS_USE_LOCAL_INFERENCE)
    message(STATUS "Building with local inference (whisper + llama + Piper)")

    # --- shared ggml options ---
    set(BUILD_SHARED_LIBS           OFF CACHE BOOL "" FORCE)
    set(GGML_BLAS                   OFF CACHE BOOL "" FORCE)
    if(APPLE)
        set(GGML_METAL                  ON  CACHE BOOL "" FORCE)
        set(GGML_METAL_EMBED_LIBRARY    ON  CACHE BOOL "" FORCE)
        set(GGML_ACCELERATE             ON  CACHE BOOL "" FORCE)
    else()
        # Linux: CPU-only, no Metal / Accelerate
        set(GGML_METAL                  OFF CACHE BOOL "" FORCE)
        set(GGML_METAL_EMBED_LIBRARY    OFF CACHE BOOL "" FORCE)
        set(GGML_ACCELERATE             OFF CACHE BOOL "" FORCE)
        set(CMAKE_POSITION_INDEPENDENT_CODE ON)
    endif()

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
    # dir.
    set(XP_WELLYS_ATC_ESPEAKNG_DATA_DIR
        "${CMAKE_BINARY_DIR}/espeak_ng-install/share/espeak-ng-data"
        CACHE PATH "Path to espeak-ng-data used at runtime (dev fallback)")

    # Path to onnxruntime prebuilt — staged next to the .xpl so it
    # resolves through @loader_path / $ORIGIN with no gymnastics.
    if(APPLE)
        set(ONNXRUNTIME_LIB_DIR
            "${SPIKES_DIR}/spike_piper/third_party/piper1-gpl/libpiper/lib/onnxruntime-osx-arm64-1.22.0/lib"
            CACHE PATH "Directory holding libonnxruntime.<ver>.dylib")
    else()
        set(ONNXRUNTIME_LIB_DIR
            "${SPIKES_DIR}/spike_piper/third_party/piper1-gpl/libpiper/lib/onnxruntime-linux-x64-1.22.0/lib"
            CACHE PATH "Directory holding libonnxruntime.so")
    endif()
else()
    message(STATUS "Building OpenAI-only (no whisper/llama/piper)")
endif()

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
    src/atc/intent_rules.cpp
    src/atc/atc_state_machine.cpp
    src/atc/atc_templates.cpp
    src/atc/phraseology_hints.cpp
    src/atc/flight_phase.cpp
    src/atc/atis_generator.cpp
    src/atc/de_phraseology.cpp
    src/atc/bzf_compliance.cpp
    src/atc/traffic_advisor.cpp
    src/atc/traffic_dialog.cpp
    src/atc/landing_sequence.cpp
    src/atc/flows/ground_operations.cpp
    src/atc/flows/flow_coordinator.cpp
    src/atc/flows/pattern_flow.cpp
    src/atc/flows/crosscountry_flow.cpp
    src/backends/manager.cpp
    src/data/airport_vrps.cpp
    src/data/airspace_db.cpp
    src/data/openair_db.cpp
    src/data/cifp_reader.cpp
    src/data/traffic_context.cpp
    src/data/traffic_geometry.cpp
    src/data/traffic_phase_classifier.cpp
    src/data/simbrief_ofp.cpp
    src/core/xplane_context.cpp
    # Model manifest is SDK-free (data + SHA256 via CommonCrypto),
    # so it lives here and the headless atc_repl tool can use it
    # too if it ever needs to verify a local model.
    src/persistence/model_manifest.cpp
    # Catalog reads data/models_catalog.json at startup and feeds the
    # manifest plus the Settings UI dropdowns. SDK-free, JSON-only.
    src/persistence/models_catalog.cpp
)
target_include_directories(xp_atc_engine PRIVATE
    ${CMAKE_SOURCE_DIR}/vendor
    ${CMAKE_SOURCE_DIR}/src
)
target_compile_definitions(xp_atc_engine PRIVATE
    XP_WELLYS_ATC_VERSION="${VERSION_STRING}"
)
if(UNIX)
    target_compile_options(xp_atc_engine PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        -fvisibility=hidden
    )
endif()
if(UNIX AND NOT APPLE)
    set_target_properties(xp_atc_engine PROPERTIES POSITION_INDEPENDENT_CODE ON)
    # OpenSSL::Crypto is needed by model_manifest.cpp (SHA256 on Linux).
    find_package(OpenSSL REQUIRED)
    target_link_libraries(xp_atc_engine PUBLIC OpenSSL::Crypto)
endif()
if(XP_WELLYS_ATC_SANITIZE)
    target_compile_options(xp_atc_engine PRIVATE
        ${XP_WELLYS_ATC_SANITIZER_FLAGS} -O1 -g
    )
endif()

# ── Platform-specific plugin sources ─────────────────────────────────────────
# Select the correct mic-permission and clipboard TUs per platform.
# On macOS these are Objective-C++ .mm files; on Linux they are plain .cpp stubs.
if(APPLE)
    set(MIC_PERMISSION_SRC src/audio/mic_permission.mm)
    set(CLIPBOARD_SRC      src/ui/clipboard.mm)
elseif(UNIX AND NOT APPLE)
    set(MIC_PERMISSION_SRC src/audio/mic_permission_linux.cpp)
    set(CLIPBOARD_SRC      src/ui/clipboard_linux.cpp)
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
    ${MIC_PERMISSION_SRC}
    # OpenAI cloud clients — libcurl + nlohmann/json, never include
    # any local backend header. Each TU carries a kBackendTag
    # constant so every inference call shows up under
    # [STT-OPENAI] / [LM-OPENAI] / [TTS-OPENAI] in Log.txt.
    # Always compiled — even in the Intel slice that does not have
    # the local backends, OpenAI is the only available path.
    src/backends/openai_common.cpp
    src/backends/openai_stt.cpp
    src/backends/openai_lm.cpp
    src/backends/openai_tts.cpp
    # Mistral cloud clients — same shape as OpenAI's (libcurl +
    # nlohmann/json), reuse the provider-agnostic helpers in
    # openai_common (WAV encode/decode, last4). Each TU carries a
    # [STT-MISTRAL] / [LM-MISTRAL] / [TTS-MISTRAL] tag so every call
    # is attributable in Log.txt. Compiled in both slices — Mistral
    # is cloud-only and works on x86_64 as well as arm64.
    src/backends/mistral_stt.cpp
    src/backends/mistral_lm.cpp
    src/backends/mistral_tts.cpp
    # Loader orchestrates verification + concrete backend load on a
    # worker thread; uses XPLMGetPluginInfo (via model_paths) so it
    # has to live in the plugin module, not the engine library.
    src/backends/loader.cpp
    # Downloader: libcurl + std::filesystem on a worker thread.
    # Plugin-only because it depends on libcurl + plugin-relative
    # paths via model_paths.
    src/backends/downloader.cpp
    src/backends/simbrief_client.cpp
    src/persistence/model_paths.cpp
    src/persistence/settings.cpp
    src/persistence/keychain.cpp
    src/ui/atc_ui.cpp
    src/ui/ui_strings.cpp
    # Platform clipboard bridge. On macOS reads NSPasteboard (AppKit);
    # on Linux shells out to wl-paste (Wayland) or xclip (X11).
    ${CLIPBOARD_SRC}
    ${IMGUI_SOURCES}
)

if(XPWELLYS_USE_LOCAL_INFERENCE)
    # Concrete local backends — pull whisper.h / llama.h / piper.h
    # transitively. Static libs come from the spike submodules added
    # above. In the OpenAI-only build these TUs are not compiled at
    # all (and the loader is built without their includes).
    target_sources(xp_wellys_atc PRIVATE
        src/backends/whisper_stt.cpp
        src/backends/llama_lm.cpp
        src/backends/piper_tts.cpp
    )
endif()

# IAudioInput concrete implementation — picked per platform. On macOS
# the CoreAudio HAL; on Linux the PulseAudio pa_simple API (works
# transparently on both PipeWire and PulseAudio servers).
# audio_recorder.cpp itself stays unconditional as the platform-neutral
# façade.
if(APPLE)
    target_sources(xp_wellys_atc PRIVATE
        src/audio/audio_input_coreaudio.cpp
    )
elseif(UNIX AND NOT APPLE)
    target_sources(xp_wellys_atc PRIVATE
        src/audio/audio_input_pulseaudio.cpp
    )
endif()

target_include_directories(xp_wellys_atc PRIVATE
    ${CMAKE_SOURCE_DIR}/sdk
    ${CMAKE_SOURCE_DIR}/sdk/XPLM
    ${CMAKE_SOURCE_DIR}/sdk/XPWidgets
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/vendor
    ${IMGUI_DIR}
    ${IMGUI_DIR}/backends
)

if(XPWELLYS_USE_LOCAL_INFERENCE)
    target_include_directories(xp_wellys_atc PRIVATE
        # Local inference backends — headers from the spike submodules.
        ${SPIKES_DIR}/spike_whisper/third_party/whisper.cpp/include
        ${SPIKES_DIR}/spike_llama/third_party/llama.cpp/include
        ${SPIKES_DIR}/spike_llama/third_party/llama.cpp/ggml/include
        ${SPIKES_DIR}/spike_llama/third_party/llama.cpp/common
        ${SPIKES_DIR}/spike_piper/third_party/piper1-gpl/libpiper/include
    )
    # Surface the flag to C++ code so loader.cpp can #ifdef the
    # local-only includes + backend instantiation calls.
    target_compile_definitions(xp_wellys_atc PRIVATE
        XPWELLYS_USE_LOCAL_INFERENCE=1
    )
endif()

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
        # AppKit is used by src/ui/clipboard.mm to read NSPasteboard.
        "-framework AppKit"
        # Security.framework is used by persistence/keychain.cpp to
        # store the OpenAI API key in the macOS Keychain. Always
        # linked — Keychain is the storage for both build modes.
        "-framework Security"
        # libcurl is used by the plugin's model downloader and by
        # the three OpenAI cloud clients. Both modes need it.
        CURL::libcurl
    )

    if(XPWELLYS_USE_LOCAL_INFERENCE)
        target_link_libraries(xp_wellys_atc PRIVATE
            # Local inference: whisper.cpp + llama.cpp build static
            # and link in directly. Piper is shared (libpiper.dylib)
            # and is resolved at runtime via @loader_path — see rpath
            # setup below.
            whisper
            common
            llama
            piper
        )

        # rpath: dev builds find dylibs in build/ and build/lib/<ort>;
        # the installed bundle finds them via @loader_path (Makefile
        # install target copies libpiper.dylib +
        # libonnxruntime.<ver>.dylib next to the .xpl).
        set_target_properties(xp_wellys_atc PROPERTIES
            BUILD_WITH_INSTALL_RPATH FALSE
            BUILD_RPATH "${CMAKE_BINARY_DIR};${ONNXRUNTIME_LIB_DIR}"
            INSTALL_RPATH "@loader_path"
        )

        # Stage the runtime dependencies next to the built .xpl so a
        # fresh `cmake --build` produces a directory the install step
        # can copy verbatim.
        add_custom_command(TARGET xp_wellys_atc POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:piper>"
                "${CMAKE_BINARY_DIR}/$<TARGET_FILE_NAME:piper>"
            COMMAND ${CMAKE_COMMAND} -E echo
                "Staged libpiper next to xp_wellys_atc.xpl"
            COMMENT "Staging libpiper.dylib alongside xp_wellys_atc.xpl"
        )
        # Copy only the actual onnxruntime dylibs (not pkgconfig + dSYM).
        # The versioned + unversioned (symlink-style) names both need
        # to be present so libpiper's
        # @rpath/libonnxruntime.1.22.0.dylib resolves.
        add_custom_command(TARGET xp_wellys_atc POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ONNXRUNTIME_LIB_DIR}/libonnxruntime.1.22.0.dylib"
                "${CMAKE_BINARY_DIR}/libonnxruntime.1.22.0.dylib"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ONNXRUNTIME_LIB_DIR}/libonnxruntime.dylib"
                "${CMAKE_BINARY_DIR}/libonnxruntime.dylib"
            COMMENT "Staging libonnxruntime dylibs alongside xp_wellys_atc.xpl"
        )
    else()
        # OpenAI-only build: no shared dylibs to stage. @loader_path
        # still applies for the install (so future shared libs would
        # be found), but nothing actually resolves through it.
        set_target_properties(xp_wellys_atc PROPERTIES
            BUILD_WITH_INSTALL_RPATH FALSE
            INSTALL_RPATH "@loader_path"
        )
    endif()
endif()

# ── Linux plugin link + compile block ────────────────────────────────────────
if(UNIX AND NOT APPLE)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(PULSE REQUIRED libpulse-simple)

    target_compile_definitions(xp_wellys_atc PRIVATE
        APL=0 IBM=0 LIN=1
        XPLM200=1 XPLM210=1 XPLM300=1 XPLM301=1 XPLM400=1
        XP_WELLYS_ATC_VERSION="${VERSION_STRING}"
    )
    target_compile_options(xp_wellys_atc PRIVATE
        -Wall -Wextra -Wno-unused-parameter
        -fvisibility=hidden
    )
    target_include_directories(xp_wellys_atc PRIVATE
        ${PULSE_INCLUDE_DIRS}
    )

    set(SDK_LIN_LIBS "${CMAKE_SOURCE_DIR}/sdk/Libraries/Lin")
    target_link_libraries(xp_wellys_atc PRIVATE
        "${SDK_LIN_LIBS}/XPLM_64.so"
        CURL::libcurl
        ${PULSE_LIBRARIES}
    )

    if(XPWELLYS_USE_LOCAL_INFERENCE)
        # $ORIGIN rpath: the plugin finds libpiper.so and libonnxruntime.so
        # staged next to the .xpl.
        set_target_properties(xp_wellys_atc PROPERTIES
            BUILD_WITH_INSTALL_RPATH FALSE
            BUILD_RPATH "\$ORIGIN:${CMAKE_BINARY_DIR}:${ONNXRUNTIME_LIB_DIR}"
            INSTALL_RPATH "\$ORIGIN"
        )
        target_link_libraries(xp_wellys_atc PRIVATE
            whisper common llama piper
        )
        add_custom_command(TARGET xp_wellys_atc POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "$<TARGET_FILE:piper>"
                "${CMAKE_BINARY_DIR}/$<TARGET_FILE_NAME:piper>"
            COMMENT "Staging libpiper.so alongside xp_wellys_atc.xpl"
        )
        # piper1-gpl's own CMake sets libpiper.so's RUNPATH to the
        # onnxruntime prebuilt dir — an absolute build-machine path that
        # doesn't exist on the end-user's machine.  Strip it to $ORIGIN so
        # libpiper.so resolves libonnxruntime.so.1 from its own directory.
        find_program(PATCHELF_EXECUTABLE patchelf)
        if(PATCHELF_EXECUTABLE)
            add_custom_command(TARGET xp_wellys_atc POST_BUILD
                COMMAND ${PATCHELF_EXECUTABLE} --set-rpath "$ORIGIN"
                        "${CMAKE_BINARY_DIR}/$<TARGET_FILE_NAME:piper>"
                COMMENT "Patching libpiper.so RPATH to $ORIGIN"
                VERBATIM
            )
        else()
            message(WARNING
                "patchelf not found — libpiper.so will keep the build-time "
                "RPATH and won't load on other machines. "
                "Fix: sudo apt install patchelf")
        endif()
        add_custom_command(TARGET xp_wellys_atc POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ONNXRUNTIME_LIB_DIR}/libonnxruntime.so.1.22.0"
                "${CMAKE_BINARY_DIR}/libonnxruntime.so.1.22.0"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ONNXRUNTIME_LIB_DIR}/libonnxruntime.so"
                "${CMAKE_BINARY_DIR}/libonnxruntime.so"
            # libpiper.so links against the SONAME libonnxruntime.so.1 —
            # the .so symlink alone is not enough; the .so.1 symlink must
            # also be present next to the .xpl.
            COMMAND ${CMAKE_COMMAND} -E create_symlink
                libonnxruntime.so.1.22.0
                "${CMAKE_BINARY_DIR}/libonnxruntime.so.1"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${ONNXRUNTIME_LIB_DIR}/libonnxruntime_providers_shared.so"
                "${CMAKE_BINARY_DIR}/libonnxruntime_providers_shared.so"
            COMMENT "Staging libonnxruntime libs alongside xp_wellys_atc.xpl"
        )
    else()
        set_target_properties(xp_wellys_atc PROPERTIES
            BUILD_WITH_INSTALL_RPATH FALSE
            INSTALL_RPATH "\$ORIGIN"
        )
    endif()
endif()

# ── CLI Executable (ohne X-Plane) ────────────────────────────────────────────
# Host-arch only — no need for universal binary on a dev tool. The
# x_atc_engine OBJECT library is compiled for whatever
# CMAKE_OSX_ARCHITECTURES the plugin targets, so for a cross-arch
# plugin build (e.g. the x86_64 slice of the universal binary built
# on an arm64 host) we cannot link an arm64 atc_repl against x86_64
# engine objects. Skip the dev tools in that case — the universal
# build pipeline runs each arch as its own configure dir and only
# needs the .xpl from each.
set(_xp_engine_arch_matches_host TRUE)
if(APPLE AND CMAKE_OSX_ARCHITECTURES)
    if(NOT "${CMAKE_HOST_SYSTEM_PROCESSOR}" IN_LIST CMAKE_OSX_ARCHITECTURES)
        set(_xp_engine_arch_matches_host FALSE)
        message(STATUS "Cross-arch build (host ${CMAKE_HOST_SYSTEM_PROCESSOR} -> "
                       "target ${CMAKE_OSX_ARCHITECTURES}); skipping atc_repl "
                       "+ unit tests")
    endif()
endif()

if(_xp_engine_arch_matches_host)
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
if(UNIX AND NOT APPLE)
    target_compile_options(atc_repl PRIVATE
        -Wall -Wextra -Wno-unused-parameter
    )
    # OpenSSL::Crypto already required above for xp_atc_engine.
    target_link_libraries(atc_repl PRIVATE OpenSSL::Crypto)
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
endif() # _xp_engine_arch_matches_host

# ── IFR headless test CLI ─────────────────────────────────────────────────────
if(_xp_engine_arch_matches_host)
add_executable(atc_ifr_repl
    $<TARGET_OBJECTS:xp_atc_engine>
    tools/atc_ifr_repl/main.cpp
    tools/atc_ifr_repl/ifr_repl.cpp
    tools/atc_repl/settings_stub.cpp
    tools/atc_repl/xplane_context_stub.cpp
)
target_include_directories(atc_ifr_repl PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/vendor
    ${CMAKE_SOURCE_DIR}/tools/atc_ifr_repl
    ${CMAKE_SOURCE_DIR}/tools/atc_repl
)
target_link_libraries(atc_ifr_repl PRIVATE CURL::libcurl)
if(APPLE)
    target_compile_options(atc_ifr_repl PRIVATE
        -Wall -Wextra -Wno-unused-parameter
    )
    set_target_properties(atc_ifr_repl PROPERTIES
        OSX_ARCHITECTURES "${CMAKE_HOST_SYSTEM_PROCESSOR}"
    )
endif()
if(UNIX AND NOT APPLE)
    target_compile_options(atc_ifr_repl PRIVATE
        -Wall -Wextra -Wno-unused-parameter
    )
    target_link_libraries(atc_ifr_repl PRIVATE OpenSSL::Crypto)
endif()
if(XP_WELLYS_ATC_SANITIZE)
    target_compile_options(atc_ifr_repl PRIVATE
        ${XP_WELLYS_ATC_SANITIZER_FLAGS} -O1 -g
    )
    target_link_options(atc_ifr_repl PRIVATE ${XP_WELLYS_ATC_SANITIZER_FLAGS})
endif()
set_target_properties(atc_ifr_repl PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}
)
endif() # _xp_engine_arch_matches_host

# ── Tests ────────────────────────────────────────────────────────────────────
if(BUILD_TESTS AND _xp_engine_arch_matches_host)
    add_subdirectory(tests)
endif()
