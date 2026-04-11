# Cross-Country — ATC Script

VFR cross-country flight (example: LSZG Grenchen → LSZB Bern-Belp).
Callsign: **Hotel Bravo Lima Uniform Kilo** (HB-LUK)

The key phrase is **"on course"** on the ready-for-departure call — it tells ATC this is a cross-country, not a pattern flight.

---

## Phase 1 — Departure (LSZG)

### 1. Ground — Taxi Request

**Pilot:**
> Grenchen Ground, Hotel Bravo Lima Uniform Kilo, request taxi.

**ATC:**
> Hotel Bravo Lima Uniform Kilo, Grenchen Ground, information Alpha, taxi to holding point runway zero six via Alpha, QNH 1013.

**Pilot (readback):**
> Taxi to holding point runway zero six via Alpha, QNH 1013, Hotel Bravo Lima Uniform Kilo.

---

### 2. Tower — Cross-Country Departure Request

**Pilot:**
> Grenchen Tower, Hotel Bravo Lima Uniform Kilo, holding short runway zero six, ready for departure, on course.

**ATC:**
> Hotel Bravo Lima Uniform Kilo, Grenchen Tower, runway zero six, cleared for takeoff, wind calm, on course approved, frequency change approved when airborne.

**Pilot (readback):**
> Cleared for takeoff runway zero six, on course, Hotel Bravo Lima Uniform Kilo.

---

### 3. Leaving Frequency

**Pilot (after takeoff):**
> Hotel Bravo Lima Uniform Kilo, leaving your frequency, good day.

**ATC:**
> Hotel Bravo Lima Uniform Kilo, good day.

---

## Phase 2 — En Route

No ATC contact. Cruise toward the destination.

---

## Phase 3 — Arrival (LSZB)

### 4. Tower — Inbound Call (via VRP)

Bern-Belp publishes visual reporting points: **November** (N), **Sierra** (S), **Whiskey** (W), **Echo** (E). Report your position **over the VRP** you're crossing — Tower will clear you into the control zone via that point.

Use a position marker (`over`, `at`, `passing`, `approaching`, `abeam`) before the VRP name.

**Pilot:**
> Bern Tower, Hotel Bravo Lima Uniform Kilo, over Whiskey, 3500 feet, inbound for landing, information Bravo.

**ATC:**
> Hotel Bravo Lima Uniform Kilo, Bern-Belp Tower, cleared to enter control zone via Whiskey, runway one four, report left downwind.

**Pilot (readback):**
> Cleared via Whiskey, runway one four, wilco report left downwind, Hotel Bravo Lima Uniform Kilo.

*Fallback without a VRP* — if you don't name a VRP, Tower gives a generic entry:

**Pilot:**
> Bern Tower, Hotel Bravo Lima Uniform Kilo, ten miles northwest, inbound for landing, information Bravo.

**ATC:**
> Hotel Bravo Lima Uniform Kilo, Bern Tower, enter left downwind runway one four, report midfield downwind.

---

### 5. Downwind

**Pilot:**
> Hotel Bravo Lima Uniform Kilo, midfield left downwind runway one four.

**ATC:**
> Hotel Bravo Lima Uniform Kilo, number one, runway one four, continue approach, report final.

**Pilot:**
> Wilco, will report final, Hotel Bravo Lima Uniform Kilo.

---

### 6. Final

**Pilot:**
> Hotel Bravo Lima Uniform Kilo, final runway one four.

**ATC:**
> Hotel Bravo Lima Uniform Kilo, runway one four, cleared to land, wind calm.

**Pilot (readback):**
> Cleared to land runway one four, Hotel Bravo Lima Uniform Kilo.

---

### 7. Runway Vacated + Taxi to Parking

LSZB has no separate ground frequency — Tower handles taxi.

**Pilot:**
> Bern Tower, Hotel Bravo Lima Uniform Kilo, clear of runway one four, request taxi to general aviation parking.

**ATC:**
> Hotel Bravo Lima Uniform Kilo, taxi to general aviation parking via Alpha, good day.

**Pilot:**
> Taxi to parking via Alpha, Hotel Bravo Lima Uniform Kilo, good day.
