# IFR & Linux Roadmap

This file tracks the IFR flight simulation feature set and the Linux port status
for the `xp_wellys_atc` fork. VFR features are maintained upstream.

---

## Phase 1 — Linux port (Ubuntu 24.04) — DONE

- PipeWire / PulseAudio mic capture (`pa_simple`)
- OpenSSL EVP SHA256 (replaces CommonCrypto)
- `$ORIGIN` rpath, `lin_x64` install directory
- `libXPLM_64.so` extracted via `make setup`
- Transponder DataRefs (`transponder_code`, `transponder_mode`)

---

## Phase 2 — IFR Ground Operations

### 2.1 SimBrief integration — 85%

- [x] Async OFP fetch + parse (destination, SID, cruise FL, registration, type)
- [x] Full navlog parsed (`NavlogFix`: ident, lat/lon, airway, alt, SID/STAR flag)
- [x] IFR tab: Pilot ID, `[Fetch OFP]`, route summary, scrollable FPL waypoint list
- [x] `ctx.ifr_destination` populated from OFP
- [ ] Slot-time check: warn if sim time is off the filed `sched_off`
- [ ] Block `REQUEST_IFR_CLEARANCE` when destination is empty
- [ ] `[Clear OFP]` button for new flight
- [ ] Local `.fms` fallback when SimBrief is offline

### 2.2 Clearance + Startup — 90%

- [x] ATIS information letter challenge before clearance
- [x] Clearance: squawk (random in configured range) + SID (CIFP) + initial altitude + destination
- [x] Engine start approval
- [x] Tower-only airports (no separate Delivery/Ground controller)
- [x] CIFP binding minimum altitude (`ifr_sid_min_alt_ft` / `ifr_sid_min_waypoint`)
- [ ] Re-clearance when pilot requests below CIFP binding minimum

### 2.3 Taxiing + Departing — 95%

- [x] Taxi clearance with passive squawk reminder ("verify squawk XXXX mode Charlie")
- [x] Squawk check at holding point: active mode C + correct code verified
- [x] Line-up-and-wait → takeoff clearance
- [x] Takeoff clearance: wind stated (not read back) + "passing Xft contact Approach on Y.YYY"
- [x] Tower → Departure/Approach freq handoff (`IFR_FREQ_HANDOFF`); pilot reads back
- [x] Departure check-in → `IFR_RADAR_CONTACT`; ATC issues SID step climbs
- [x] Direct-to last SID fix + step1 FL + cruise FL clearances via `poll_sid_climb()`
- [x] Radar handoff at TMA upper boundary (openair_db) → Centre (`IFR_ENROUTE_CRUISE`)
- [x] Controller name + frequency from `atc.dat` TRACON at 3-D aircraft position
- [ ] Departure re-clearance enforcing CIFP binding minimum

---

## Phase 3 — En-route — 60%

- [x] `IFR_ENROUTE_CRUISE` state: pilot on Centre, no SID step-climb re-trigger
- [x] Centre check-in (`INITIAL_CALL_APPROACH`): "radar contact." — stays in cruise state
- [x] En-route direct-to shortcut: "N111RC, direct XAMUR, when able."
      (fired ~90-120 s after Centre check-in; first non-SID/STAR navlog fix >20 NM ahead)
- [x] Proactive descent clearance on destination TMA entry (no pilot request needed):
      openair_db CTA/UIR→TMA class transition triggers
      "N111RC, descend flight level 80, contact Nice Approach on 120.350."
      → `IFR_APPROACH_CONTACT`
- [x] Cross-track deviation alert: "confirm routing, you appear off track."
      (>5 NM off filed navlog, 3-minute cooldown between warnings)
- [ ] Centre altitude reassignments (level-off, step-climb during cruise)
- [ ] En-route traffic separation (speed / heading / altitude adjustments)
- [ ] Destination airport change mid-flight → automatic transition to Approach contact
- [ ] TMA descent fallback when `airspace.txt` is absent

---

## Phase 4 — Approach + Landing — not started

- [ ] Approach initial contact: inbound call, information letter, current level
- [ ] Descent clearance: "descend 4000 ft QNH 1013, expect ILS runway 22"
- [ ] ILS / RNAV established report → landing clearance
- [ ] Landing clearance with wind + runway
- [ ] Missed approach / go-around: "fly runway heading, climb 4000 ft"
- [ ] New pilot intents: `ESTABLISHED_ILS`, `ESTABLISHED_RNAV`, `GOING_AROUND_IFR`
- [ ] STAR shortcut from navlog: "direct XAMUR, descend FL100, expect ILS runway 04"

---

## Phase 5 — Post-landing — not started

- [ ] Runway vacated → contact Ground
- [ ] Taxi to stand / parking via holding points
- [ ] Engine shutdown acknowledgement
- [ ] Prerequisite: Dijkstra routing on apt.dat 1201/1202 node/edge graph

---

## Supporting Infrastructure

| Item | Status |
|---|---|
| Transcript log (`<plugin>/Resources/transcript.log`) | done |
| Cross-track error monitoring (SimBrief navlog) | done (Phase 3) |
| `earth_fix.dat` / `earth_nav.dat` waypoint resolution | not started |
| Taxi routing — Dijkstra on apt.dat graph | not started |
| Navigraph data support | not started |

---

## Wishlist

- Airport data source indicator in UI (Global apt.dat / Custom Scenery / Navigraph)
  — helps diagnose false ATIS frequencies (e.g. AFIS coded as row-50 ATIS)
- IFR holding patterns: "hold at XAMUR, inbound 270, right turns, EFC 1430"
- En-route traffic separation: RVSM 1000 ft / 5 NM conflict resolution
- TMA descent fallback when `airspace.txt` is absent (distance-to-destination trigger)
