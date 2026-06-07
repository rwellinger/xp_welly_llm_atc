# M12 — Linux Port — Phase 1: Audio Input Interface

## Context

Cross-platform port on branch `feat/linux-port`. Today, `src/audio/audio_recorder.cpp` talks to CoreAudio HAL directly (`AudioComponentFindNext`, `AudioUnitRender`, ~250 lines of Apple-specific setup). For Linux we need a second implementation, but engine code (`atc_session`, STT backends) must not branch on platform.

The good news: `src/audio/audio_player.cpp` already uses `XPLMPlayPCMOnBus` via the X-Plane SDK and is portable as-is. **Only input needs porting.**

This phase is a **pure refactor on macOS** — extract today's CoreAudio code behind a strategy interface, mirror the pattern used by the three backend families (`i_speech_to_text.hpp` etc.). No behavior change. No Linux code yet.

## Goal

Introduce `audio::IAudioInput` strategy interface, move existing CoreAudio implementation into a dedicated TU, refactor `audio_recorder.cpp` into a thin façade. Plugin must behave identically on macOS after this phase.

## Steps

1. Create `src/audio/i_audio_input.hpp` — SDK-free interface with:
   - `open()` / `close()` lifecycle
   - `start_recording()` / `stop_recording()`
   - `take_pcm()` returning `std::vector<int16_t>` (mono, device-native rate)
   - `sample_rate_hz()`, `buffer_samples()`, `duration_seconds()`
   - Factory: `std::unique_ptr<IAudioInput> make_audio_input();` declared at bottom of header
2. Create `src/audio/audio_input_coreaudio.cpp`:
   - Move the entire CoreAudio HAL setup (today inside `audio_recorder.cpp`) into a `class CoreAudioInput : public IAudioInput`
   - Implement `make_audio_input()` here — returns `std::make_unique<CoreAudioInput>()`
3. Refactor `src/audio/audio_recorder.cpp` to be a thin façade:
   - Hold a `static std::unique_ptr<IAudioInput> input_;`
   - `init()` calls `make_audio_input()` then `input_->open()`
   - All other functions forward to the interface
   - **Public API in `audio_recorder.hpp` stays byte-for-byte identical** — no caller changes
4. Update CMake:
   - Add `audio_input_coreaudio.cpp` to the plugin module sources gated by `if(APPLE)`
   - `audio_recorder.cpp` stays in the plugin module sources unconditionally
   - The `i_audio_input.hpp` interface header can live in the engine OBJECT lib path (SDK-free)

## Files Affected

- **New:** `src/audio/i_audio_input.hpp`
- **New:** `src/audio/audio_input_coreaudio.cpp`
- **Modified:** `src/audio/audio_recorder.cpp` (becomes façade)
- **Unchanged public API:** `src/audio/audio_recorder.hpp`
- **Modified:** `CMakeLists.txt`

## CMake Changes

Add the new files. Gate `audio_input_coreaudio.cpp` with `if(APPLE)` so non-Apple builds will later pick up a different file.

## Commit Message

```
refactor(audio): extract input behind IAudioInput interface

Move CoreAudio HAL setup into a dedicated TU implementing a SDK-free
strategy interface. audio_recorder is now a thin façade. No behavior
change. Sets the stage for PortAudio on Linux.

Closes M12 Phase 1.
```

## Validation

- `make all` green on macOS (build + lint + test)
- Plugin loads in X-Plane, PTT recording → STT → response pipeline must work end-to-end exactly as before
- Compare `Log.txt` audio-init lines against pre-refactor — same sample rate detection, same callback count

## Depends On

- Phase 0 (audit test) — to be sure the refactor cannot accidentally break backend invariants

## Estimated Effort

~½ day
