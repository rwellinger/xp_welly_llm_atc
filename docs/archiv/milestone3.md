# MILESTONE M3 — Whisper STT Integration

Read CLAUDE.md completely before starting.
M2 must be complete before this milestone begins.

## Goal

At the end of this milestone:
- WAV buffer is sent to OpenAI Whisper after PTT release
- Transcript text is displayed in ImGui
- Pipeline: PTT hold → record → release → transcribe → display
- No ATC logic yet
- All changes committed to git

---

## Task 1 — whisper_client

Implement `whisper_client::transcribe()`:

```cpp
namespace whisper_client {
    void init();
    void stop();

    // Async: calls callback on main-thread-safe queue when done
    void transcribe_async(
        std::vector<uint8_t> wav_data,
        std::function<void(std::string transcript, bool success)> callback
    );
}
```

**HTTP call** (libcurl, multipart/form-data):
```
POST https://api.openai.com/v1/audio/transcriptions
Authorization: Bearer <settings::get_api_key()>
Content-Type: multipart/form-data

file=<wav_data as "audio.wav", type audio/wav>
model=whisper-1
language=en
```

**Implementation notes:**
- Run on `std::thread` — never block the X-Plane main thread
- Use `CURLOPT_POSTFIELDS` with multipart via `curl_mime_*` API
- Parse JSON response: `{ "text": "..." }` using `nlohmann::json`
- On success: call callback with transcript string, `success=true`
- On failure (HTTP error, curl error, no API key): call callback with error description, `success=false`
- Never log the API key in any error message or debug output

**Delivering result to main thread:**
Use a thread-safe queue: `std::queue` protected by `std::mutex`. Drain queue in flight loop callback each frame, call pending callbacks on main thread.

---

## Task 2 — atc_session: Integrate Whisper

Update `on_ptt_released()`:
- After `audio_recorder::stop_recording()`, call `audio_recorder::encode_wav()`
- Transition state to `PROCESSING`
- Call `whisper_client::transcribe_async(wav_data, callback)`
- In callback (main thread):
  - If success: store transcript, transition to `IDLE`, log via `XPLMDebugString`
  - If failure: log error, transition to `IDLE`

Add minimum recording duration gate: if `audio_recorder::duration_seconds() < 0.5f`, discard and return to `IDLE` without calling Whisper.

---

## Task 3 — atc_ui: Transcript Display

Update ImGui window:

**Tab: Transcript** (new tab, or replace Status tab placeholder):
- Scrolling list of `TranscriptEntry` items
- Pilot entries shown in white: `[sim_time] You: <text>`
- ATC entries shown in cyan (comes in M5, prepare the layout now)
- Auto-scroll to bottom on new entry
- "Clear" button

Keep Status tab as-is from M1.

---

## Task 4 — Verify

1. Ensure `api_key_saved: true` and valid key in Keychain (from M1)
2. Load plugin in X-Plane 12
3. Hold PTT, say "Zurich Ground, HB-ABC, request taxi"
4. Release PTT
5. Confirm:
   - Status shows "⟳ PROCESSING" during API call
   - Transcript tab shows your spoken text after response
   - X-Plane Log.txt shows transcript
6. Test failure case: temporarily set invalid key, confirm graceful error (no crash, error shown in ImGui)

---

## Commit

```bash
git add -A
git commit -m "feat(M3): Whisper STT — async transcription, main-thread callback queue, transcript display in ImGui"
```
