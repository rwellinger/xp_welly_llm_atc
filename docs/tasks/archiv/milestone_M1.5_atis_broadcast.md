# MILESTONE M1.5 — ATIS Broadcast + Weather Context

Read CLAUDE.md completely before starting.

## Goal

At the end of this milestone:
- Plugin generates realistic ATIS broadcasts using X-Plane weather data
- ATIS plays automatically when pilot tunes COM to airport's ATIS frequency
- Own ATIS letter (Alpha, Bravo, ...) increments on significant weather/runway changes
- ATIS letter is referenced in ATC initial call templates
- All weather data needed for realistic ATIS is available in XPlaneContext

---

## Background

X-Plane's built-in ATIS/ATC does NOT expose its active runway or ATIS information via DataRefs or SDK APIs. To provide a consistent VFR communication flow, we generate our own ATIS as "source of truth":

```
Pilot tunes ATIS freq → hears our ATIS (runway, wind, QNH, letter)
    → tunes Ground/Tower → "with information Alpha"
    → ATC knows the letter, runway matches our determination
```

Prerequisite: Runway awareness is already implemented (active runway from wind + apt.dat).

---

## Task 1 — Extended Weather DataRefs (`src/xplane_context.hpp/.cpp`)

Add new weather fields to `XPlaneContext`:

```cpp
// Additional weather for ATIS
float visibility_m = 9999.0f;          // sim/weather/visibility_reported_m
float cloud_base_ft_msl = 99999.0f;    // sim/weather/cloud_base_msl_m[0] * 3.28084
int   cloud_type = 0;                  // sim/weather/cloud_type[0]  (0=clear,1=few,2=scattered,3=broken,4=overcast)
float temperature_c = 15.0f;           // sim/weather/temperature_sealevel_c
float dewpoint_c = 10.0f;              // sim/weather/dewpoi_sealevel_c
```

Register these DataRefs in `init()`, read in `update()`.

---

## Task 2 — ATIS Frequency from apt.dat (`src/xplane_context.hpp/.cpp`)

Extend apt.dat parsing in `build_towered_cache()` to also extract ATIS/AWOS frequencies:

- Code `50` (old format) / `1050` (X-Plane 12): ATIS/AWOS/ASOS frequency
- Store in new cache: `static std::unordered_map<std::string, float> atis_freq_cache_`
- Add to `XPlaneContext`: `float atis_freq_mhz = 0.0f;`
- Populate in `update()` throttled block alongside runway data

---

## Task 3 — ATIS Letter Management (`src/atis_generator.hpp/.cpp`)

New module for ATIS generation:

```cpp
namespace atis_generator {

void init();
void stop();

// Current ATIS letter ('A' through 'Z', wraps around)
char current_letter();

// Called every ~1s from xplane_context update or flight loop.
// Increments letter if active runway or significant weather changed.
void check_for_update(const xplane_context::XPlaneContext& ctx);

// Generate full ATIS text for the nearest airport
std::string generate_atis_text(const xplane_context::XPlaneContext& ctx);

// Returns true if the given COM frequency matches the ATIS freq of nearest airport
bool is_tuned_to_atis(const xplane_context::XPlaneContext& ctx);

} // namespace atis_generator
```

### ATIS Letter increment triggers:
- Active runway changes
- Wind direction changes > 30°
- QNH changes > 1 hPa
- Visibility category changes (>10km / 5-10km / <5km)

### ATIS Text Template:

```
"{airport} Information {letter}. 
Runway in use {runway}. 
Wind {wind}. 
Visibility {visibility}. 
{clouds}
Temperature {temp}, dewpoint {dewpoint}. 
QNH {qnh}. 
Advise on initial contact you have information {letter}."
```

Variables:
- `{letter}` — current ATIS letter name ("Alpha", "Bravo", ...)
- `{visibility}` — formatted: "10 kilometers" or "4500 meters"
- `{clouds}` — "Sky clear" / "Few clouds at 3000 feet" / "Broken at 1500 feet" / etc.
- `{temp}`, `{dewpoint}` — integer celsius

---

## Task 4 — ATIS Playback Trigger (`src/atc_session.cpp`)

Detect when pilot tunes COM to ATIS frequency and auto-play:

```
Every update cycle:
  if atis_generator::is_tuned_to_atis(ctx):
    if not already_playing_atis AND not recently_played (cooldown 30s):
      text = atis_generator::generate_atis_text(ctx)
      → tts_client → audio_player
      set cooldown timer
```

- ATIS plays once when frequency is first tuned (not looping endlessly)
- 30-second cooldown before it can play again (prevents spam on freq switching)
- ATIS does NOT play while PTT pipeline is active (RECORDING/PROCESSING/PLAYING)

---

## Task 5 — Update Initial Call Templates (`data/atc_templates.json`)

Add `{atis_letter}` variable to templates. Update initial call responses:

```json
"IDLE": {
  "INITIAL_CALL_GROUND": {
    "response": "{callsign}, {airport} Ground, information {atis_letter} current, runway {runway}, QNH {qnh}.",
    "next_state": "GROUND_CONTACT",
    "requires_readback": false
  }
}
```

Update `build_vars()` in `atc_state_machine.cpp` to include:
```cpp
{"atis_letter", std::string(1, atis_generator::current_letter())}
```

---

## Task 6 — UI Status Display (`src/atc_ui.cpp`)

Add ATIS info to Status tab:

```
ATIS: Information Alpha | 127.850 MHz
```

---

## Task 7 — Files to modify

| File | Action |
|---|---|
| `src/xplane_context.hpp` | **MODIFY** — add weather + ATIS freq fields |
| `src/xplane_context.cpp` | **MODIFY** — add weather DataRefs, parse ATIS freq from apt.dat |
| `src/atis_generator.hpp` | **NEW** — ATIS generation header |
| `src/atis_generator.cpp` | **NEW** — ATIS text generation, letter management |
| `src/atc_session.cpp` | **MODIFY** — ATIS playback trigger when freq tuned |
| `src/atc_state_machine.cpp` | **MODIFY** — add atis_letter to build_vars |
| `src/atc_ui.cpp` | **MODIFY** — show ATIS info in status tab |
| `data/atc_templates.json` | **MODIFY** — add {atis_letter} to initial call templates |
| `src/main.cpp` | **MODIFY** — add atis_generator init/stop |
| `CMakeLists.txt` | **MODIFY** — add atis_generator.cpp |
| `CLAUDE.md` | **MODIFY** — document ATIS module |

---

## Task 8 — Verify

1. `make lint && make build`
2. Start at towered airport (LSZG), tune COM to ATIS frequency
   → Expect: ATIS plays with correct runway, wind, QNH, letter
3. Retune to ATIS → should NOT replay within 30s cooldown
4. Wait 30s, retune → should replay
5. Tune to Ground freq, PTT → "Grenchen Ground, HB-LUK, with information Alpha, request taxi"
   → Expect: response references "information Alpha current"
6. Change weather significantly → ATIS letter should increment
7. Status tab shows current ATIS letter and frequency

---

## Commit

```bash
git add -A
git commit -m "feat(M1.5): ATIS broadcast with weather data + information letter"
```
