# MILESTONE M1 — X-Plane Context + Settings + Keychain

Read CLAUDE.md completely before starting.
The scaffold from START.md must be complete and compiling before this milestone begins.

## Goal

At the end of this milestone:
- `xplane_context` reads all DataRefs correctly and displays live values in ImGui
- `settings` loads/saves `data/settings.json`
- API key can be entered in ImGui Settings tab and is stored in macOS Keychain
- `nearest_airport_id` and `is_towered_airport` are correctly derived
- All changes committed to git

---

## Task 1 — xplane_context: Full DataRef Implementation

Implement all fields of `XPlaneContext` as defined in CLAUDE.md.

DataRefs to use:
```
sim/flightmodel/position/latitude
sim/flightmodel/position/longitude
sim/flightmodel/position/elevation        → altitude_ft_msl (convert m→ft)
sim/flightmodel/position/groundspeed      → kts (convert m/s → kts)
sim/flightmodel/position/indicated_airspeed
sim/flightmodel/position/vh_ind_fpm
sim/flightmodel/position/psi              → heading_true
sim/flightmodel/position/y_agl            → on_ground if < 0.5m and groundspeed < 5kts
sim/cockpit2/radios/actuators/com1_frequency_hz_833
sim/cockpit2/radios/actuators/com2_frequency_hz_833
sim/cockpit2/radios/actuators/audio_com_selection → active_com
sim/aircraft/view/acf_ICAO                → aircraft_icao
```

For `engines_running`: check `sim/cockpit2/engine/indicators/N1_percent[0]` > 5.0f.

For `nearest_airport_id` and `is_towered_airport`:
- Use `XPLMGetNavAidInfo` with type `xplm_Nav_Airport`
- Find nearest within 10nm of current lat/lon
- `is_towered_airport`: check if any navaid of type `xplm_Nav_LocalizerILS` or tower frequency exists at that airport — use `XPLMFindNavAid` with the airport ICAO and check for associated COM navaid

Update `XPlaneContext` in the flight loop callback (every frame).

---

## Task 2 — settings: JSON + Keychain

Implement `settings::init()`:
- Create `data/` directory if not exists
- Load `data/settings.json` if exists, otherwise create with defaults from CLAUDE.md
- If `api_key_saved: true`, load API key from Keychain into memory (`settings::get_api_key()`)

Implement `settings::save()`:
- Write all fields except API key to `data/settings.json`
- Never write the API key to any file

Implement Keychain functions:
```cpp
namespace settings {
    bool        save_api_key(const std::string& key);   // SecKeychainItemAdd / SecKeychainItemModifyContent
    std::string load_api_key();                          // SecKeychainFindGenericPassword
    void        delete_api_key();                        // SecKeychainItemDelete
    std::string get_api_key();                           // returns in-memory cached key
}
```

Keychain service name: `"xp_wellys_atc"`, account name: `"openai_api_key"`.

Use `Security.framework` directly — no third-party wrappers.

---

## Task 3 — atc_ui: Status Panel + Settings Tab

Extend the ImGui window with two tabs:

**Tab: Status**
Display live `XPlaneContext` values (for development/debugging):
- Nearest airport + is_towered flag
- Active COM frequency
- Position (lat/lon)
- Groundspeed, IAS, altitude
- On ground flag
- Aircraft ICAO

**Tab: Settings**
- API Key field (masked with `ImGuiInputTextFlags_Password`)
- "Save Key" button → calls `settings::save_api_key()`, shows "Saved ✓" for 2 seconds
- "Delete Key" button → calls `settings::delete_api_key()`
- Pilot callsign text input
- Volume slider (0.0–1.0)
- TTS Voice combo (alloy / echo / fable / onyx / nova / shimmer)
- GPT fallback checkbox
- Debug logging checkbox
- "Save Settings" button → calls `settings::save()`

PTT binding UI comes in M7 — leave as placeholder label "PTT: not yet configured".

---

## Task 4 — Verify

1. Load plugin in X-Plane 12
2. Open "Welly's ATC" window
3. Confirm Status tab shows correct live values while flying/taxiing
4. Enter a test API key in Settings tab, click Save — verify it does NOT appear in `data/settings.json`
5. Restart X-Plane — verify key is loaded back from Keychain automatically

---

## Commit

```bash
git add -A
git commit -m "feat(M1): xplane_context DataRefs, settings JSON, Keychain API key storage, ImGui status+settings tabs"
```
