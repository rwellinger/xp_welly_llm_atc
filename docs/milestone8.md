# MILESTONE M8 — Polish, CTAF, Frequency Awareness, Error Handling

Read CLAUDE.md completely before starting.
M7 must be complete before this milestone begins.

## Goal

At the end of this milestone:
- CTAF/Unicom flow works correctly at non-towered airports
- Plugin is frequency-aware (active COM drives ATC context)
- All error paths are handled gracefully — no crashes, clear user feedback
- Debug logging mode is useful for troubleshooting
- Plugin is ready for personal use
- All changes committed to git, version tagged

---

## Task 1 — Frequency Awareness

Currently the plugin ignores which COM frequency is tuned. Fix this:

**In `xplane_context`:**
- Read active COM frequency each frame (`com1_freq_mhz` or `com2_freq_mhz` based on `active_com`)
- Derive `FrequencyType` from the tuned frequency:

```cpp
enum class FrequencyType {
    UNKNOWN,
    DELIVERY,       // typically 121.x delivery freqs
    GROUND,         // 121.x ground freqs
    TOWER,          // varies by airport
    APPROACH,       // 119.x–125.x approach freqs
    UNICOM,         // 122.8, 123.0 common CTAF
    CTAF,           // airport-specific CTAF
    ATIS,           // 127.x–128.x
};
```

Map frequency ranges to types (approximate heuristic — not a full navdata lookup):
- 121.6–121.9: likely Ground
- 122.8, 123.0: Unicom/CTAF
- 118.0–136.0: Tower/Approach (distinguish by `is_towered_airport`)

**In `atc_state_machine`:**
- Include `FrequencyType` in context passed to `process()`
- If `FrequencyType == UNICOM` or `CTAF`: force `UNICOM_ACTIVE` flow regardless of state
- If `FrequencyType == GROUND`: allow GROUND_CONTACT / TAXI_CLEARED states only
- If `FrequencyType == TOWER`: allow TOWER_CONTACT / DEPARTURE_CLEARED / PATTERN_ENTRY / LANDING_CLEARED states only

**In `atc_ui`:** Show tuned frequency and derived type in Status tab.

---

## Task 2 — CTAF / Unicom Flow Polish

At non-towered airports (`is_towered_airport == false`):

- All PTT inputs → UNICOM_ACTIVE state
- ATC state machine does not issue clearances
- Response: acknowledge the self-announce with traffic awareness text only
- Example: "Traffic in the area, HB-ABC reported downwind runway 28."
- After acknowledgement: immediately return to IDLE (no state progression)

At towered airports, if CTAF frequency is tuned and airport is closing/closed:
- Treat as non-towered (UNICOM flow)
- This is an edge case — acceptable to leave as future improvement with a TODO comment

---

## Task 3 — Error Handling

Implement graceful handling for all known failure modes:

| Failure | Behavior |
|---|---|
| No API key configured | Block PTT, show persistent warning in ImGui: "⚠ API key not set" |
| Whisper API error (HTTP 4xx/5xx) | Log error, show in ImGui transcript: "[Error: transcription failed]", return to IDLE |
| Whisper API timeout (>15s) | Cancel request, return to IDLE, show "[Error: transcription timed out]" |
| GPT API error | Use "Say again, [callsign]." fallback response instead |
| TTS API error | Skip audio, show ATC response text in transcript only, return to IDLE |
| Audio playback error (decode failure) | Log error, return to IDLE |
| No microphone device found | Block PTT, show warning: "⚠ No microphone found" |
| Recording too short (< 0.5s) | Silently discard, return to IDLE (no error shown) |

All errors: log via `XPLMDebugString` with `[xp_wellys_atc][ERROR]` prefix.

---

## Task 4 — Debug Logging Mode

When `settings::debug_logging == true`:

Log the following via `XPLMDebugString` for each PTT cycle:
```
[xp_wellys_atc][DEBUG] PTT pressed
[xp_wellys_atc][DEBUG] Recording stopped: 2.3s, 36800 samples
[xp_wellys_atc][DEBUG] WAV encoded: 73644 bytes
[xp_wellys_atc][DEBUG] Whisper response: "Zurich Ground, HB-ABC, request taxi runway 28"
[xp_wellys_atc][DEBUG] Intent: REQUEST_TAXI (confidence=0.90), runway=28, callsign=HB-ABC
[xp_wellys_atc][DEBUG] State: IDLE → GROUND_CONTACT
[xp_wellys_atc][DEBUG] ATC response text: "HB-ABC, Zurich Ground, information X, runway 28, QNH 1013, taxi to holding point 28 via Alpha."
[xp_wellys_atc][DEBUG] TTS response: 48320 bytes MP3
[xp_wellys_atc][DEBUG] Playback started
[xp_wellys_atc][DEBUG] Playback finished, state → IDLE
```

Never log the API key or any part of it.

---

## Task 5 — Final ImGui Polish

- Add plugin version string to window title: "Welly's ATC v1.0.0"
- Add "About" section in Settings tab: version, GitHub link (placeholder)
- Ensure ImGui window is resizable and remembers its last position/size (store in settings.json: `ui_window_x`, `ui_window_y`, `ui_window_w`, `ui_window_h`)
- Transcript tab: show frequency next to each entry if available
- Status bar: show total session stats (transcriptions made, API calls)

---

## Task 6 — Version + Release

Create `VERSION.txt`:
```
1.0.0
```

Update `CMakeLists.txt` to read `VERSION.txt` and define `XP_WELLYS_ATC_VERSION` compile-time string.

Final commit and tag:

```bash
git add -A
git commit -m "feat(M8): frequency awareness, CTAF flow, error handling, debug logging, UI polish"
git tag -a v1.0.0 -m "v1.0.0 — initial release"
```

---

## Done

The plugin is complete and ready for personal use.
Future work (not in scope now): GitHub Actions CI, Windows build, public release, API key distribution model for multi-user.
