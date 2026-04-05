# MILESTONE M2 — PTT + Audio Recording

Read CLAUDE.md completely before starting.
M1 must be complete before this milestone begins.

## Goal

At the end of this milestone:
- Push-to-talk via configurable keyboard key is functional
- Microphone audio is captured during PTT hold and encoded to WAV in memory
- WAV buffer size and recording duration are displayed in ImGui for verification
- No API calls yet — audio pipeline only
- All changes committed to git

---

## Task 1 — ptt_input: Keyboard PTT

Implement `ptt_input::init()`:
- Register key sniffer via `XPLMRegisterKeySniffer`
- Default PTT key: VK code from `settings::ptt_key_vk` (-1 = not configured)
- On key down: call `atc_session::on_ptt_pressed()`
- On key up: call `atc_session::on_ptt_released()`
- If `ptt_key_vk == -1`, PTT is disabled (log warning)

Key sniffers in X-Plane receive virtual key codes. Use standard macOS VK codes.
Do not capture keys when an ImGui text input field is focused.

---

## Task 2 — audio_recorder

Implement Core Audio microphone capture:

**Init:**
- Create `AudioComponentInstance` using `kAudioUnitSubType_HALOutput`
- Configure for input: 16kHz, mono, 16-bit signed integer PCM
- Set render callback to append samples to `std::vector<int16_t> buffer_`

**Interface:**
```cpp
namespace audio_recorder {
    void init();
    void stop();
    void start_recording();   // clears buffer, starts AudioUnit
    void stop_recording();    // stops AudioUnit, buffer is ready
    std::vector<uint8_t> encode_wav();  // encode buffer_ to WAV bytes in memory
    float duration_seconds(); // buffer size / sample_rate
    size_t buffer_samples();
}
```

**WAV encoding** (in memory, no temp files):
Write standard 44-byte WAV header followed by raw PCM samples into `std::vector<uint8_t>`:
- ChunkID: "RIFF", Format: "WAVE"
- Subchunk1: "fmt ", PCM format (1), 1 channel, 16000 Hz, 16-bit
- Subchunk2: "data", + raw int16 samples

---

## Task 3 — atc_session: PTT State Machine

Implement the PTT state machine in `atc_session`:

```cpp
enum class PTTState { IDLE, RECORDING, PROCESSING, PLAYING };
```

`on_ptt_pressed()`:
- If state is not `IDLE`: ignore (log "PTT blocked, state=X")
- Transition to `RECORDING`
- Call `audio_recorder::start_recording()`
- Update ImGui status: "● RECORDING"

`on_ptt_released()`:
- If state is not `RECORDING`: ignore
- Call `audio_recorder::stop_recording()`
- Log duration and buffer size via `XPLMDebugString`
- Transition to `IDLE` (Whisper not yet implemented — will change in M3)

---

## Task 4 — atc_ui: Recording Status

Update ImGui Status tab:
- Show current PTT state prominently: `IDLE` / `● REC` / `⟳ PROCESSING` / `▶ PLAYING`
- Show last recording: duration in seconds, buffer size in samples
- Show last WAV buffer size in bytes (after encode)

---

## Task 5 — Verify

1. Load plugin in X-Plane 12
2. Configure PTT key in Settings (hardcode a test key VK code directly in settings.json for now — proper binding UI comes in M7)
3. Hold PTT key — confirm "● REC" status appears
4. Release PTT — confirm duration and buffer sizes appear in ImGui and X-Plane Log.txt
5. Hold < 0.5s and confirm it still records (no minimum duration gate yet)

---

## Commit

```bash
git add -A
git commit -m "feat(M2): PTT keyboard input, Core Audio mic recording, WAV in-memory encoding, PTT state machine"
```
