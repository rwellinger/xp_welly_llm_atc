# xp_wellys_atc — ATC Call Reference

## Concept

The plugin captures pilot voice via push-to-talk, transcribes it with OpenAI
Whisper, classifies the pilot's intent (rule-based parser with GPT-4o-mini
fallback), and drives a VFR ATC state machine. Responses are generated from
JSON templates filled with live X-Plane context (callsign, airport, active
runway, wind, QNH, ATIS letter) and played back via OpenAI TTS.

The ATC state machine mirrors a real towered VFR flow: pilot contacts Ground
or Tower, receives clearances, transitions through pattern or cross-country
departure, and either lands back or leaves the frequency en route. Every state
only accepts the intents that make sense for that phase — anything else is
rejected with a "say again" style response. Flight-phase guards derived from
aircraft DataRefs prevent illegal calls (e.g. requesting takeoff while
airborne).

Airports without a Tower fall back to a simple CTAF self-announce loop.

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
| `READBACK` | *"Taxi runway 26 via Alpha, N123AB."* | *"N123AB, readback correct."* |

### State: `TAXI_CLEARED`

Taxiing to the holding point.

| Pilot Intent | Example Pilot Call | ATC Response |
|---|---|---|
| `READY_FOR_DEPARTURE` | *"Springfield Tower, N123AB, holding short runway 26, ready for departure."* | *"N123AB, Springfield Tower, runway 26, cleared for takeoff, wind 240 at 8, report left downwind."* |
| `READY_FOR_DEPARTURE_VFR` | *"Tower, N123AB, ready for departure, on course northbound."* | *"N123AB, Tower, runway 26, cleared for takeoff, wind 240 at 8, on course approved, frequency change approved when airborne."* |
| `INITIAL_CALL_TOWER` | *"Springfield Tower, N123AB."* | *"N123AB, Tower, runway 26, hold short, number one."* |
| `READBACK` | *"Taxi runway 26 via Alpha, N123AB."* | *"N123AB, readback correct, contact tower when ready."* |

### State: `TOWER_CONTACT`

Tower has acknowledged but no clearance issued yet.

| Pilot Intent | Example Pilot Call | ATC Response |
|---|---|---|
| `READY_FOR_DEPARTURE` | *"N123AB, ready for departure runway 26."* | *"N123AB, runway 26, cleared for takeoff, wind 240 at 8, report left downwind."* |
| `READY_FOR_DEPARTURE_VFR` | *"N123AB, ready for departure, on course north."* | *"N123AB, runway 26, cleared for takeoff, wind 240 at 8, on course approved, frequency change approved when airborne."* |
| `REQUEST_LANDING` | *"N123AB, request landing runway 26."* | *"N123AB, number one, runway 26, report final."* |
| `REQUEST_TOUCH_AND_GO` | *"N123AB, request touch and go runway 26."* | *"N123AB, runway 26, cleared touch and go, wind 240 at 8."* |
| `REPORT_POSITION` | *"N123AB, five miles south."* | *"N123AB, number one, runway 26, report final."* |
| `READBACK` | *"Cleared for takeoff 26, N123AB."* | *"N123AB, readback correct."* |

### State: `DEPARTURE_CLEARED`

Airborne after takeoff clearance. Pattern or cross-country.

| Pilot Intent | Example Pilot Call | ATC Response |
|---|---|---|
| `REPORT_POSITION_DOWNWIND` | *"N123AB, midfield left downwind runway 26."* | *"N123AB, number one, runway 26, continue approach, report final."* |
| `REPORT_POSITION_BASE` | *"N123AB, turning left base runway 26."* | *"N123AB, number one, runway 26, continue approach."* |
| `REPORT_POSITION_FINAL` | *"N123AB, final runway 26."* | *"N123AB, runway 26, cleared to land, wind 240 at 8."* |
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

### State: `LANDING_CLEARED`

Cleared to land — no more clearances until runway vacated or go-around.

| Pilot Intent | Example Pilot Call | ATC Response |
|---|---|---|
| `RUNWAY_VACATED` | *"N123AB, clear of runway 26."* | *"N123AB, contact ground on 121.9, good day."* |
| `GO_AROUND` | *"N123AB, going around."* | *"N123AB, roger, fly runway heading, climb and maintain pattern altitude, re-enter left downwind runway 26."* |

### State: `EN_ROUTE`

Cross-country cruise — tower is not on frequency. No responses until a new
airport's frequency is tuned (context switches automatically back to `IDLE`).

---

## User Manual

- [User Manual (English)](xpwellysatc_manual_en.md)
- [Benutzerhandbuch (Deutsch)](xpwellysatc_manual_de.md)

---

## Template Variables

All responses are filled at runtime from X-Plane context:

| Variable | Source |
|---|---|
| `{callsign}` | `settings.pilot_callsign` |
| `{airport}` | Nearest airport (geometric or frequency-driven) |
| `{runway}` | Wind-selected active runway |
| `{wind}` | `sim/weather/wind_*` direction + speed |
| `{qnh}` | `sim/weather/barometer_sealevel_inhg` → hPa |
| `{atis_letter}` | Current ATIS info letter (Alpha–Zulu) |
| `{pattern_direction}` | Runway pattern side (left/right) |
| `{entry_vrp}` | Detected VRP from `airport_vrps.json` |
| `{frequency}` | Ground/handoff frequency from `apt.dat` |
