# M12 — Linux Port — Phase 2: PortAudio Implementation (macOS-validated)

## Context

Cross-platform port on branch `feat/linux-port`. Phase 1 introduced `audio::IAudioInput` and put CoreAudio behind it. This phase adds a **second** implementation backed by PortAudio. We deliberately build and validate PortAudio on macOS first, before relying on it for Linux — the maintainer cannot test Linux directly, so any class of bug we can catch on macOS pays double.

PortAudio internally wraps WASAPI (Win), ALSA/PulseAudio/JACK (Linux), and CoreAudio (Mac), giving us one codepath for all current and future target OSes.

**Library choice — PortAudio over miniaudio:** The maintainer specifically asked for PortAudio. miniaudio (single-header public domain) was considered as an alternative; revisit only if PortAudio's dylib bundling becomes painful.

## Goal

Add `audio_input_portaudio.cpp` that implements `IAudioInput` against PortAudio. Make the active input backend selectable via a CMake option, default `coreaudio` on macOS. Build, link, and validate on macOS by flipping the option.

## Steps

1. Add PortAudio to the build:
   - Preferred: git submodule under `vendor/portaudio/` (consistent with how whisper.cpp/llama.cpp are vendored)
   - Alternative: CMake `FetchContent` if submodule churn is annoying
2. Create `src/audio/audio_input_portaudio.cpp`:
   - `class PortAudioInput : public IAudioInput`
   - `Pa_Initialize` in `open()`, `Pa_Terminate` in `close()`
   - `Pa_OpenStream` with `paInt16`, 1 channel, device-native sample rate from `Pa_GetDeviceInfo(default)->defaultSampleRate`
   - Static callback writes into a `std::vector<int16_t>` under a mutex (same pattern as today's CoreAudio code)
   - `make_audio_input()` returns `std::make_unique<PortAudioInput>()` — but **only** when this TU is the chosen factory provider
3. CMake — make the factory pluggable:
   - New option `XPWELLYS_AUDIO_INPUT_BACKEND` with allowed values `coreaudio` (Apple default) and `portaudio`
   - The matching `audio_input_*.cpp` becomes the source of `make_audio_input()`
   - Only one input TU is compiled into a given build
4. Stage `libportaudio.dylib` next to `libpiper.dylib` / `libonnxruntime.*.dylib` so `make install` finds it with `@loader_path` rpath
5. On macOS, build with `-DXPWELLYS_AUDIO_INPUT_BACKEND=portaudio` and run end-to-end in X-Plane. Compare:
   - Recording start/stop log lines
   - Detected sample rate (PortAudio may pick a different default than HAL)
   - Captured PCM peak level vs reference
   - Whisper STT round-trip latency
6. Restore the default Apple build to `coreaudio` so `main`-merge does not change observable behavior for existing macOS users.

## Files Affected

- **New:** `src/audio/audio_input_portaudio.cpp`
- **New:** `vendor/portaudio/` (submodule) or CMake `FetchContent` block
- **Modified:** `CMakeLists.txt`

## CMake Changes

```cmake
set(XPWELLYS_AUDIO_INPUT_BACKEND "coreaudio" CACHE STRING
    "Audio input backend: coreaudio or portaudio")
set_property(CACHE XPWELLYS_AUDIO_INPUT_BACKEND PROPERTY STRINGS coreaudio portaudio)

if(XPWELLYS_AUDIO_INPUT_BACKEND STREQUAL "coreaudio")
    target_sources(xp_wellys_atc PRIVATE src/audio/audio_input_coreaudio.cpp)
elseif(XPWELLYS_AUDIO_INPUT_BACKEND STREQUAL "portaudio")
    target_sources(xp_wellys_atc PRIVATE src/audio/audio_input_portaudio.cpp)
    target_link_libraries(xp_wellys_atc PRIVATE portaudio)
endif()
```

Default for non-Apple platforms in later phases will be `portaudio`.

## Commit Message

```
feat(audio): add PortAudio input backend (macOS validation)

Second IAudioInput implementation. CMake option XPWELLYS_AUDIO_INPUT_BACKEND
selects coreaudio (default on Apple) or portaudio. Validated on macOS by
flipping the option — STT round-trip identical. Sets up the Linux build
in M12 Phase 4 to reuse this code with zero further changes.

Closes M12 Phase 2.
```

## Validation

- Both default (`coreaudio`) and `-DXPWELLYS_AUDIO_INPUT_BACKEND=portaudio` builds compile and link
- PortAudio build: PTT recording → STT in X-Plane returns the same transcript as CoreAudio build for the same spoken phrase
- `make test` + `make sanitize` green for the PortAudio build
- THIRD_PARTY.md updated with PortAudio attribution (defer if minor)

## Depends On

- Phase 1 (audio interface) — required

## Estimated Effort

~1 day
