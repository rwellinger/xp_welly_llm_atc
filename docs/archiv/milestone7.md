# MILESTONE M7 — Joystick PTT + Settings Persistence

Read CLAUDE.md completely before starting.
M6 must be complete before this milestone begins.

## Goal

At the end of this milestone:
- PTT can be bound to keyboard key or joystick button via ImGui (press-to-capture)
- Bindings and all settings are persisted correctly to `data/settings.json`
- All changes committed to git

---

## Task 1 — ptt_input: Joystick Button Support

Add joystick button polling to `ptt_input`:

**DataRef to poll each flight loop frame:**
```
sim/joystick/joy_buttons   (type: byte array, 3200 elements)
```

- Index from `settings::ptt_joystick_button` (−1 = disabled)
- Compare current vs previous frame value to detect press (0→1) and release (1→0)
- On press: call `atc_session::on_ptt_pressed()`
- On release: call `atc_session::on_ptt_released()`

If both `ptt_key_vk` and `ptt_joystick_button` are configured, either one triggers PTT (OR logic).

---

## Task 2 — atc_ui: PTT Binding Panel

Replace the "PTT: not yet configured" placeholder in Settings tab with a full binding UI:

**Keyboard binding:**
```
[Current key: Space (VK 49)]  [Bind keyboard key]
```
- "Bind keyboard key" button: enters capture mode
- In capture mode: display "Press any key..." and grab next key sniffer event
- Store VK code in `settings::ptt_key_vk`, save settings
- "Clear" button to unbind (set to −1)

**Joystick button binding:**
```
[Current button: Button 12]  [Bind joystick button]
```
- "Bind joystick button" button: enters capture mode
- In capture mode: poll `sim/joystick/joy_buttons` each frame, detect first button that goes from 0→1
- Store index in `settings::ptt_joystick_button`, save settings
- "Clear" button to unbind (set to −1)

Show capture mode status clearly (e.g. yellow text "Waiting for input...").
Capture mode times out after 10 seconds if no input detected.

---

## Task 3 — settings: Validate Persistence

Verify that all settings fields round-trip correctly through JSON:
- `ptt_key_vk`, `ptt_joystick_button`
- `tts_voice`, `tts_model`, `whisper_model`, `gpt_model`
- `gpt_fallback_enabled`, `debug_logging`
- `pilot_callsign`
- `active_com`
- `volume`
- `api_key_saved` flag (never the key itself)

Implement `settings::load()` with safe defaults for any missing field (do not crash on partial JSON).

---

## Task 4 — Verify

1. Open Settings tab, click "Bind keyboard key", press Space → confirm binding saved and shown
2. Restart X-Plane → confirm binding still active
3. Click "Bind joystick button", press a joystick button → confirm binding saved
4. Test both PTT sources trigger recording
5. Change callsign, save settings, restart → confirm callsign persists and appears in ATC responses
6. Change TTS voice, test response → confirm different voice

---

## Commit

```bash
git add -A
git commit -m "feat(M7): joystick PTT, press-to-capture key/button binding UI, full settings persistence"
```
