# MILESTONE M6 — TTS + Audio Playback (Full Loop)

Read CLAUDE.md completely before starting.
M5 must be complete before this milestone begins.

## Goal

At the end of this milestone:
- ATC responses are spoken aloud via OpenAI TTS
- Full pipeline is functional: PTT → record → transcribe → parse → ATC logic → speak
- Volume control works
- All changes committed to git

---

## Task 1 — tts_client

Implement `tts_client::speak_async()`:

```cpp
namespace tts_client {
    void init();
    void stop();
    void speak_async(
        const std::string& text,
        std::function<void(std::vector<uint8_t> mp3_data, bool success)> callback
    );
}
```

**HTTP call:**
```
POST https://api.openai.com/v1/audio/speech
Authorization: Bearer <key>
Content-Type: application/json

{
  "model": "tts-1",
  "input": "<text>",
  "voice": "<settings::tts_voice>",
  "response_format": "mp3"
}
```

- Response body is raw MP3 binary — read via `CURLOPT_WRITEFUNCTION` into `std::vector<uint8_t>`
- Same async/callback pattern as `whisper_client`
- On failure: callback with empty vector, `success=false`

---

## Task 2 — audio_player

Implement MP3 decode and playback via macOS system frameworks only (no ffmpeg, no external libs).

```cpp
namespace audio_player {
    void init();
    void stop();
    void play(std::vector<uint8_t> mp3_data, float volume);
    bool is_playing();
}
```

**MP3 → PCM decode using AudioToolbox:**
```cpp
// Write mp3_data to a CFData / AudioFileStream
// Use ExtAudioFileRef or AudioFileOpenWithCallbacks to decode
// Output: LPCM, 44100Hz (or native), stereo, float32
```

**Playback via Core Audio:**
- Use `AudioUnit` output (`kAudioUnitSubType_DefaultOutput`)
- Render callback supplies decoded PCM frames
- Apply `volume` as a linear scalar on samples before output
- Set `is_playing_ = false` when PCM buffer is exhausted

**Thread safety:** `play()` may be called from main thread. Render callback runs on audio thread. Use `std::atomic<bool>` for `is_playing_` and a lock-free buffer (or `std::mutex`-protected queue).

---

## Task 3 — atc_session: Full Pipeline

Update session after `ATCResponse` text is available:

```
ATCResponse text
    ↓
Transition to PLAYING
    ↓
tts_client::speak_async(text, callback)
    ↓
callback: audio_player::play(mp3_data, settings::volume)
    ↓
Poll audio_player::is_playing() in flight loop
    ↓
When playback done: transition to IDLE
```

Status display during each phase:
- `RECORDING`: "● REC"
- `PROCESSING`: "⟳ Transcribing..." / "⟳ ATC thinking..."
- `PLAYING`: "▶ ATC speaking..."
- `IDLE`: "Ready"

---

## Task 4 — Verify

Full end-to-end test at a towered airport:

1. Load plugin, ensure API key is set
2. Hold PTT, speak ATC request, release
3. Confirm sequence in ImGui: REC → PROCESSING → PLAYING → IDLE
4. Confirm ATC voice plays through speakers
5. Confirm PTT is blocked while ATC is speaking
6. Adjust volume slider in Settings, confirm effect on next response
7. Test with voice set to "alloy" and "onyx" — confirm different voices
8. Test error case: disable network, confirm graceful fallback to IDLE (no crash)

---

## Commit

```bash
git add -A
git commit -m "feat(M6): OpenAI TTS integration, Core Audio MP3 playback, full PTT→voice pipeline complete"
```
