# US / Canada ATC Procedure Examples

This HOWTO walks through three common VFR scenarios using the plugin's **US**
region (FAA / NAV CANADA phraseology). Switch to it in the Settings tab
(`Region: US`) before starting — templates, flight rules, and CTAF phrasing
all change at runtime.

Throughout this document:

- **Callsign:** *N123AB* → spoken *"November One Two Three Alpha Bravo"*
  (after the first ATC response, ATC abbreviates to *"Three Alpha Bravo"*).
  Set it in the Settings tab as `N123AB`.
- **Example towered airport:** **KPAO — Palo Alto** (runway 31, left traffic).
- **Example non-towered airport:** **KWVI — Watsonville** (runway 20, CTAF
  122.975).
- **Example Approach facility:** *NORCAL Approach* (generic — the plugin uses
  the airport's own Approach ident when available).

When talking to ATC, speak clearly on PTT. The plugin transcribes via Whisper,
so natural English — *"runway three one"* or *"runway thirty one"* — both work.

---

## Scenario 1 — Inbound Pattern (Towered)

Arriving at **KPAO** from the south, 10 NM out, 2,500 ft. You already copied
the ATIS (letter *Bravo*) and the active runway is **31**.

### Step 1 — Tune the Tower frequency

Tune COM1 to **Palo Alto Tower 118.600**. The status bar should show
`TOWER` as the frequency type.

### Step 2 — Initial call inbound

> **Pilot:** *Palo Alto Tower, November One Two Three Alpha Bravo, 10 miles south, 2,500, with Bravo, inbound for landing.*
>
> **ATC:** *N123AB, Palo Alto Tower, enter left downwind runway 31, report midfield.*
>
> **Pilot (readback):** *Enter left downwind runway 31, will report midfield, Three Alpha Bravo.*

State transition: `IDLE` → `PATTERN_ENTRY`.

### Step 3 — Report midfield downwind

Fly the 45° entry to the left downwind. Abeam the numbers:

> **Pilot:** *Palo Alto Tower, Three Alpha Bravo, midfield left downwind runway 31.*
>
> **ATC:** *Three Alpha Bravo, number one, runway 31, continue approach, report final.*

### Step 4 — Turning final

> **Pilot:** *Three Alpha Bravo, final runway 31.*
>
> **ATC:** *Three Alpha Bravo, runway 31, cleared to land, wind 290 at 8.*
>
> **Pilot (readback):** *Cleared to land runway 31, Three Alpha Bravo.*

State transition: `PATTERN_ENTRY` → `LANDING_CLEARED`.

### Step 5 — Runway vacated

After landing and exiting the runway:

> **Pilot:** *Palo Alto Tower, Three Alpha Bravo, clear of runway 31.*
>
> **ATC:** *Three Alpha Bravo, contact Ground on 121.700, good day.*
>
> **Pilot:** *Ground 121.700, Three Alpha Bravo, good day.*

### Step 6 — Taxi to the ramp

Tune COM1 to **Palo Alto Ground 121.700**.

> **Pilot:** *Palo Alto Ground, Three Alpha Bravo, clear of 31, taxi to the ramp.*
>
> **ATC:** *Three Alpha Bravo, taxi to the ramp via Alpha, good day.*

State returns to `IDLE`.

**Key US phraseology in this scenario:**

- Airport + Tower, not *"Tower XY"*
- *"Cleared to land, wind 290 at 8"* — identical to EU
- *"Contact Ground on 121.700"* — no *"QNH"*, just the frequency
- Callsign abbreviated to last three words after initial contact

---

## Scenario 2 — Cross-Country with Flight Following

Departing **KPAO** VFR to **KSQL — San Carlos**. You want radar advisories
from NORCAL Approach along the way.

### Phase 1 — Ground (KPAO)

Tune COM1 to **Palo Alto Ground 121.700**.

> **Pilot:** *Palo Alto Ground, N123AB, at the ramp with Bravo, ready to taxi.*
>
> **ATC:** *N123AB, Palo Alto Ground, information Bravo current, runway 31, taxi via Alpha, hold short runway 31, altimeter 29.92.*
>
> **Pilot (readback):** *Runway 31, taxi via Alpha, hold short runway 31, altimeter 29.92, Three Alpha Bravo.*

State: `IDLE` → `TAXI_CLEARED`.

Note the US-specific bits the plugin fills in:

- *"runway 31, taxi via Alpha, hold short runway 31"* — FAA AIM 4-3-18
  requires explicit hold-short on every runway crossing, including the
  destination.
- *"altimeter 29.92"* — not *"QNH"*.

### Phase 2 — Ready for Departure

At the hold short line:

> **Pilot:** *Palo Alto Ground, Three Alpha Bravo, holding short runway 31, ready for takeoff.*
>
> **ATC:** *Three Alpha Bravo, contact Tower on 118.600.*
>
> **Pilot:** *Contact Tower 118.600, Three Alpha Bravo.*

Tune COM1 to **Palo Alto Tower 118.600**.

### Phase 3 — Tower Clearance (VFR on course)

Because you announced *"on course"* / *"VFR northbound"*, the plugin
classifies this as `READY_FOR_DEPARTURE_VFR` and Tower releases you to the
en-route frequency immediately:

> **Pilot:** *Palo Alto Tower, Three Alpha Bravo, holding short runway 31, ready for takeoff, VFR to San Carlos, on course.*
>
> **ATC:** *Three Alpha Bravo, Palo Alto Tower, runway 31, cleared for takeoff, wind 290 at 8, on course approved, frequency change approved.*
>
> **Pilot (readback):** *Cleared for takeoff runway 31, on course approved, frequency change approved, Three Alpha Bravo.*

State: `TAXI_CLEARED`/`TOWER_CONTACT` → `DEPARTURE_CLEARED`.

### Phase 4 — Leave Tower Frequency

Once airborne, clear of the Class D surface area:

> **Pilot:** *Palo Alto Tower, Three Alpha Bravo, leaving frequency, good day.*
>
> **ATC:** *Three Alpha Bravo, squawk 1200, good day.*

State: `DEPARTURE_CLEARED` → `EN_ROUTE`.

*(In the US, VFR squawk is **1200**, not 7000.)*

### Phase 5 — Request Flight Following

Tune COM1 to **NORCAL Approach** (the frequency printed on the VFR
sectional for the area you are in — the plugin's status bar should show
`APPROACH`).

> **Pilot:** *NORCAL Approach, November One Two Three Alpha Bravo, 2,500, VFR San Carlos, request flight following.*
>
> **ATC:** *N123AB, NORCAL Approach, squawk 4527, ident, say altitude.*
>
> **Pilot (readback):** *Squawk 4527 and ident, 2,500, Three Alpha Bravo.*

State transitions through `EN_ROUTE` → `APPROACH_CONTACT`.

Dial the squawk (4527) in the transponder and press IDENT.

### Phase 6 — En Route (silent)

While under flight following, ATC may stay silent or call traffic. The
plugin does not simulate traffic calls — you can stay on frequency and
listen. If you want to cancel advisory service:

> **Pilot:** *NORCAL Approach, Three Alpha Bravo, leaving frequency.*
>
> **ATC:** *Three Alpha Bravo, radar service terminated, squawk VFR, frequency change approved.*

State: `APPROACH_CONTACT` → `EN_ROUTE`.

*"Squawk VFR"* in the US means set **1200** again.

### Phase 7 — Arrival at KSQL

KSQL (San Carlos) is a **Class D towered** airport. Tune **KSQL Tower**
from the sectional, then continue with the **Inbound Pattern** flow from
Scenario 1 (initial call inbound → downwind → final → land → taxi).

---

## Scenario 3 — Non-Towered / CTAF (Bonus)

Arriving at **KWVI — Watsonville** (non-towered, CTAF **122.975**),
runway **20**. Tune the CTAF; the plugin's status bar shows `CTAF` or
`UNICOM`. On CTAF there is no ATC — you *self-announce* and the plugin
echoes a plausible acknowledgement only. **Airport name appears twice**:
once at the start ("_Watsonville traffic_") and once at the end
("…_Watsonville._") — that's FAA AIM 4-1-9 style and the US templates
do it automatically.

### Step 1 — 10 miles out

> **Pilot:** *Watsonville traffic, November One Two Three Alpha Bravo, 10 miles north, 2,500, inbound full stop runway 20, Watsonville.*

### Step 2 — Downwind

> **Pilot:** *Watsonville traffic, Three Alpha Bravo, midfield left downwind runway 20, full stop, Watsonville.*

### Step 3 — Base

> **Pilot:** *Watsonville traffic, Three Alpha Bravo, left base runway 20, Watsonville.*

### Step 4 — Final

> **Pilot:** *Watsonville traffic, Three Alpha Bravo, final runway 20, full stop, Watsonville.*

### Step 5 — Clear of runway

> **Pilot:** *Watsonville traffic, Three Alpha Bravo, clear of runway 20, Watsonville.*

The plugin's `SELF_ANNOUNCE` template wraps your message with the airport
name as both prefix and suffix automatically — what you hear back in the
transcript panel is the plugin's echo of the format, not a controller
reply. There are no clearances on CTAF.

---

## Quick Reference — EU vs US Phrases

| Context | EU (ICAO) | US (FAA / NAV CANADA) |
|---|---|---|
| Altimeter setting | *"QNH 1013"* | *"Altimeter 29.92"* |
| Taxi clearance | *"taxi to holding point runway 31 via Alpha"* | *"taxi via Alpha, hold short runway 31"* |
| VFR squawk | *"squawk 7000"* | *"squawk 1200"* |
| Non-towered advisory | UNICOM / AFIS | CTAF (airport as prefix and suffix) |
| Flight following | (not standard) | *"request flight following"* → Approach/Center |
| Callsign style | Full phonetic retained | Abbreviated to last 3 after initial reply |

---

## Troubleshooting while Testing

- **Plugin still says "QNH" or "holding point":** the region toggle didn't
  take effect. In Settings tab → `Region` → pick `US`. The status line
  should confirm *"Region changed to US — templates reloaded"*. Verify
  the file `data/regions/us/atc_templates.json` exists in your installed
  plugin folder.
- **ATC rejects taxi / landing requests with "unable, you appear to
  be airborne":** the flight-phase guard kicked in. Check the Status bar
  for current flight phase; land or stop first.
- **"Request flight following" rejected:** must be in `CLIMB`, `CRUISE`,
  or `PATTERN` phase and on an **APPROACH** frequency. Not on Tower,
  not on the ground.
- **CTAF self-announce sounds off:** US template wraps the airport name
  around your announcement. If you hear your own airport name only once,
  you may still be on the EU region — double-check the Settings tab.
- **Callsign not recognised:** set `Callsign (Registration)` in Settings
  to your N-number (e.g. `N123AB`). The plugin converts it to phonetic
  (*"November One Two Three Alpha Bravo"*) for matching Whisper output.
