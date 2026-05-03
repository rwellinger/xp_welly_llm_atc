# xp_wellys_atc — ATC Call Reference

## Concept

The plugin captures pilot voice via push-to-talk, transcribes it with
**whisper.cpp** (Metal-accelerated, fully local), classifies the pilot's
intent through a **rule-based parser plus an always-on local Llama 3.2 3B
classifier** that also repairs Whisper transcription artefacts ("take of"
→ "take off"), and drives a VFR ATC state machine. Responses are generated
from JSON templates filled with live X-Plane context (callsign, airport,
active runway, wind, QNH, ATIS letter) and played back via **Piper** (local,
CPU + onnxruntime). All inference runs on Apple Silicon — no cloud, no API
keys, no runtime network traffic. Models (~2 GB) are downloaded from
HuggingFace once on first launch.

The ATC state machine mirrors a real towered VFR flow: pilot contacts Ground
or Tower, receives clearances, transitions through pattern or cross-country
departure, and either lands back or leaves the frequency en route. Every state
only accepts the intents that make sense for that phase — anything else is
rejected with a "say again" style response. Flight-phase guards derived from
aircraft DataRefs prevent illegal calls (e.g. requesting takeoff while
airborne).

Airports without a Tower fall back to a simple CTAF self-announce loop.

### Regions

The plugin supports two phraseology sets selectable in the Settings tab:

- **EU** — ICAO phraseology, QNH in hPa, "taxi to holding point", VRP-based
  arrival clearances, CTAF self-announce with airport as prefix only.
- **US** — FAA / NAV CANADA phraseology (covers United States and Canada),
  Altimeter in inHg, "taxi via Alpha, hold short runway X", squawk 1200,
  `REQUEST_FLIGHT_FOLLOWING` for VFR advisory service, CTAF self-announce with
  airport name as both prefix and suffix.

Region-specific templates live under `data/regions/{eu,us}/`. Switching the
region hot-reloads templates, flight rules, and VRPs without restarting
X-Plane. The example ATC responses below all use the EU region.

---

## Tower Calls per State

Example callsign: **N123AB**, runway **26**, airport **Springfield**.

### State: `IDLE`

Initial contact before any clearance exists.

| Pilot Intent | Example Pilot Call | ATC Response |
|---|---|---|
| `INITIAL_CALL_GROUND` | *"Springfield Ground, N123AB, at south ramp, request taxi."* | *"N123AB, Springfield Ground, information Bravo current, runway 26, QNH 1013."* |
| `INITIAL_CALL_TOWER` | *"Springfield Tower, N123AB."* | *"N123AB, Springfield Tower, go ahead."* |
| `INITIAL_CALL_INBOUND` | *"Springfield Tower, N123AB, ten miles south, inbound for landing, information Bravo."* | *"N123AB, Springfield Tower, enter left downwind runway 26, report midfield downwind."* |
| `INITIAL_CALL_INBOUND_VRP` | *"Bern Tower, N123AB, over Whiskey, 3500 feet, inbound for landing, information Bravo."* | *"N123AB, Bern Tower, cleared to enter control zone via Whiskey, runway 14, report left downwind."* |
| `REQUEST_TAXI` | *"Springfield Ground, N123AB, request taxi."* | *"N123AB, Springfield Ground, information Bravo current, taxi to holding point runway 26 via Alpha, QNH 1013."* |
| `READY_FOR_DEPARTURE` | *"Springfield Tower, N123AB, holding short runway 26, ready for departure."* | *"N123AB, Springfield Tower, runway 26, cleared for takeoff, wind 240 at 8, report left downwind."* |
| `READY_FOR_DEPARTURE_VFR` | *"Springfield Tower, N123AB, holding short runway 26, ready for departure, VFR northbound, on course."* | *"N123AB, Springfield Tower, runway 26, cleared for takeoff, wind 240 at 8, on course approved, frequency change approved when airborne."* |
| `RADIO_CHECK` | *"Springfield Tower, N123AB, radio check."* | *"N123AB, Springfield Tower, reading you five by five."* |

### State: `GROUND_CONTACT`

After initial call to Ground.

| Pilot Intent | Example Pilot Call | ATC Response |
|---|---|---|
| `REQUEST_TAXI` | *"N123AB, request taxi."* | *"N123AB, taxi to holding point runway 26 via Alpha, QNH 1013."* |
| `READBACK` | *"Taxi runway 26 via Alpha, N123AB."* | *(silent — ICAO standard: no acknowledgement of a correct readback)* |

### State: `TAXI_CLEARED`

Taxiing to the holding point. Ground keeps control of the aircraft on the
manoeuvring area; the tower handoff happens only when the pilot reports
"ready for departure" at the holding point — not as part of the taxi readback.

| Pilot Intent | Example Pilot Call | ATC Response |
|---|---|---|
| `READY_FOR_DEPARTURE` | *"Springfield Ground, N123AB, holding short runway 26, ready for departure."* | *"N123AB, roger, contact Tower on 120.100."* (→ `TOWER_CONTACT`) |
| `READY_FOR_DEPARTURE_VFR` | *"Ground, N123AB, holding short runway 26, ready for departure, VFR northbound."* | *"N123AB, roger, contact Tower on 120.100."* (→ `TOWER_CONTACT`) |
| `INITIAL_CALL_TOWER` | *"Springfield Tower, N123AB."* | *"N123AB, Tower, runway 26, hold short, number one."* |
| `READBACK` | *"Taxi runway 26 via Alpha, N123AB."* | *(silent)* |

### State: `TOWER_CONTACT`

Tower has acknowledged but no clearance issued yet.

| Pilot Intent | Example Pilot Call | ATC Response |
|---|---|---|
| `READY_FOR_DEPARTURE` | *"N123AB, ready for departure runway 26."* | *"N123AB, runway 26, cleared for takeoff, wind 240 at 8, report left downwind."* |
| `READY_FOR_DEPARTURE_VFR` | *"N123AB, ready for departure, on course north."* | *"N123AB, runway 26, cleared for takeoff, wind 240 at 8, on course approved, frequency change approved when airborne."* |
| `REQUEST_LANDING` | *"N123AB, request landing runway 26."* | *"N123AB, number one, runway 26, report final."* |
| `REQUEST_TOUCH_AND_GO` | *"N123AB, request touch and go runway 26."* | *"N123AB, runway 26, cleared touch and go, wind 240 at 8."* |
| `REPORT_POSITION` | *"N123AB, five miles south."* | *"N123AB, number one, runway 26, report final."* |
| `READBACK` | *"Cleared for takeoff 26, N123AB."* | *(silent)* |

### State: `DEPARTURE_CLEARED`

Airborne after takeoff clearance. Pattern or cross-country.

| Pilot Intent | Example Pilot Call | ATC Response |
|---|---|---|
| `REPORT_POSITION_DOWNWIND` | *"N123AB, midfield left downwind runway 26."* | *"N123AB, number one, runway 26, continue approach, report final."* |
| `REPORT_POSITION_BASE` | *"N123AB, turning left base runway 26."* | *"N123AB, number one, runway 26, continue approach."* |
| `REPORT_POSITION_FINAL` | *"N123AB, final runway 26."* | *"N123AB, runway 26, cleared to land, wind 240 at 8."* |
| `READBACK` | *"Cleared for takeoff runway 26, on course, N123AB."* | *(silent)* |
| `REQUEST_FREQUENCY` | *"Tower, N123AB, request frequency change."* | *"N123AB, frequency change approved, good day."* (→ `EN_ROUTE`) |
| `LEAVING_FREQUENCY` | *"N123AB, leaving frequency, good day."* | *"N123AB, good day."* (→ `EN_ROUTE`) |

### State: `PATTERN_ENTRY`

In the circuit after inbound clearance.

| Pilot Intent | Example Pilot Call | ATC Response |
|---|---|---|
| `REPORT_POSITION_DOWNWIND` | *"N123AB, midfield left downwind runway 26."* | *"N123AB, number one, runway 26, continue approach, report final."* |
| `REPORT_POSITION_BASE` | *"N123AB, turning left base runway 26."* | *"N123AB, number one, runway 26, continue approach."* |
| `REPORT_POSITION_FINAL` | *"N123AB, final runway 26."* | *"N123AB, runway 26, cleared to land, wind 240 at 8."* |
| `REQUEST_LANDING` | *"N123AB, request landing runway 26."* | *"N123AB, runway 26, cleared to land, wind 240 at 8."* |
| `REQUEST_TOUCH_AND_GO` | *"N123AB, request touch and go runway 26."* | *"N123AB, runway 26, cleared touch and go, wind 240 at 8."* |
| `GO_AROUND` | *"N123AB, going around."* | *"N123AB, roger, fly runway heading, climb and maintain pattern altitude, re-enter left downwind runway 26."* |
| `READBACK` | *"Cleared via Whiskey, runway 14, wilco report left downwind, N123AB."* | *(silent)* |

### State: `TOUCH_AND_GO_CLEARED`

After touch-and-go clearance — pilot can continue in the pattern or vacate.

| Pilot Intent | Example Pilot Call | ATC Response |
|---|---|---|
| `REPORT_POSITION_DOWNWIND` | *"N123AB, midfield left downwind runway 26."* | *"N123AB, number one, runway 26, continue approach, report final."* |
| `REPORT_POSITION_BASE` | *"N123AB, turning left base runway 26."* | *"N123AB, number one, runway 26, continue approach."* |
| `REPORT_POSITION_FINAL` | *"N123AB, final runway 26."* | *"N123AB, runway 26, cleared to land, wind 240 at 8."* |
| `REQUEST_LANDING` | *"N123AB, request full stop runway 26."* | *"N123AB, runway 26, cleared to land, wind 240 at 8."* |
| `REQUEST_TOUCH_AND_GO` | *"N123AB, request another touch and go."* | *"N123AB, runway 26, cleared touch and go, wind 240 at 8."* |
| `GO_AROUND` | *"N123AB, going around."* | *"N123AB, roger, fly runway heading, re-enter left downwind runway 26."* |
| `RUNWAY_VACATED` | *"N123AB, clear of runway 26."* | *"N123AB, contact ground on 121.9, good day."* |
| `READBACK` | *"Cleared touch and go runway 26, N123AB."* | *(silent)* |

### State: `LANDING_CLEARED`

Cleared to land — no more clearances until runway vacated or go-around.

| Pilot Intent | Example Pilot Call | ATC Response |
|---|---|---|
| `RUNWAY_VACATED` | *"N123AB, clear of runway 26."* | *"N123AB, contact ground on 121.9, good day."* |
| `REQUEST_TAXI_PARKING` | *"Tower, N123AB, runway vacated, request taxi to parking."* | *"N123AB, runway vacated, taxi to general aviation parking via Alpha, good day."* (tower-only airports — Tower handles taxi after landing) |
| `GO_AROUND` | *"N123AB, going around."* | *"N123AB, roger, fly runway heading, climb and maintain pattern altitude, re-enter left downwind runway 26."* |
| `READBACK` | *"Cleared to land runway 26, N123AB."* | *(silent)* |

### State: `EN_ROUTE`

Cross-country cruise — tower is not on frequency. No responses until a new
airport's frequency is tuned (context switches automatically back to `IDLE`).

### Universal Intents (valid in every state)

A handful of intents are accepted regardless of the active state:

| Pilot Intent | Example Pilot Call | Effect |
|---|---|---|
| `NEGATIVE_CORRECTION` | *"Negative, N123AB, request VFR departure on course."* / *"Disregard, ..."* / *"No correction, ..."* | Reverts state one step (e.g. `DEPARTURE_CLEARED → TOWER_CONTACT`), so the pilot can re-issue the request when ATC misinterpreted them. ATC: *"N123AB, roger, correction noted, say intentions."* |
| `UNABLE` | *"Unable, N123AB."* | Acknowledged with *"N123AB, roger."* No state change. |
| `INAPPROPRIATE_LANGUAGE` | (any rude wording) | Polite first warning; escalation on repeats. The original request still gets processed on the first warning. |
| `TRAFFIC_IN_SIGHT` / `TRAFFIC_NEGATIVE_CONTACT` / `TRAFFIC_LOOKING` | *"Traffic in sight, N123AB."* | Only valid when the controller has just issued a traffic advisory; routed via the parallel traffic dialog. |

---

## User Manual

- [User Manual (English)](xpwellysatc_manual_en.md)
- [Benutzerhandbuch (Deutsch)](xpwellysatc_manual_de.md)

---

## Scenario Testing (`make test`)

The headless CLI `build/atc_repl` can batch-run JSON scenario files to
regression-test the engine without X-Plane. `make test` builds the CLI and
runs every `testscripts/*.json`; non-zero exit means at least one step
assertion failed.

Each scenario file sets a single `XPlaneContext` and a list of pilot `say`
steps. A step is either a plain string (execute only) or an object with the
fields below. Between scenarios the ATC state machine and engine counters
are reset so scenarios stay isolated.

| Field | Purpose |
|---|---|
| `text` | Pilot transcript. Omit for set-only or wait-only steps. |
| `expect` | Case-insensitive substring that must appear in the ATC response. |
| `expect_not` | Case-insensitive substring that must **not** appear. Requires `expect` or `expect_state` on the same step. |
| `expect_state` | Asserts `atc_state_machine::state_name(get_state())` after the step. |
| `quality` | Float mapped to `engine::Input.quality` (default `1.0f`). Values < `0.3f` trigger "say again". |
| `set` | Object of context fields applied **before** `text` — e.g. `{"com": 118.6, "freq_type": "TOWER"}`. |
| `wait_sec` | Integer. Ticks `flight_phase::update` and `atc_state_machine::check_auto_correction` once per simulated second. Drives delayed auto-corrections. |
| `note` | Log-only line printed before the step. |

```json
{
  "name": "LSZH Ground — taxi",
  "region": "EU",
  "context": {
    "airport": "LSZH", "towered": true, "on_ground": true,
    "com": 121.800, "freq_type": "GROUND",
    "runway": "28", "callsign": "November One Two Three Alpha Bravo"
  },
  "say": [
    { "text": "Zurich Ground N123AB requesting taxi", "expect": "taxi" },
    "Taxi to holding point runway two eight N123AB"
  ]
}
```

Supported `freq_type` values: `DELIVERY`, `GROUND`, `TOWER`, `APPROACH`,
`UNICOM`, `CTAF`, `ATIS`, `UNKNOWN`. `region` is `EU` or `US` (default `EU`).
All `context` fields are optional — omitted fields use sensible GA defaults.

---

## Template Variables

All responses are filled at runtime from X-Plane context:

| Variable | Source |
|---|---|
| `{callsign}` | `settings.pilot_callsign` |
| `{airport}` | Nearest airport (geometric or frequency-driven) |
| `{runway}` | Wind-selected active runway |
| `{wind}` | `sim/weather/wind_*` direction + speed |
| `{qnh}` | `sim/weather/barometer_sealevel_inhg` → hPa (EU region) |
| `{altimeter}` | `sim/weather/barometer_sealevel_inhg`, 2 decimals (US region) |
| `{atis_letter}` | Current ATIS info letter (Alpha–Zulu) |
| `{pattern_direction}` | Runway pattern side (left/right) |
| `{entry_vrp}` | Detected VRP from `airport_vrps.json` |
| `{frequency}` | Ground/handoff frequency from `apt.dat` |
