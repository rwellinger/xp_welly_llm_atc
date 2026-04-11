# Cross-Country LSZG → LSZB — ATC Test Script

VFR-Überlandflug von **Grenchen (LSZG)** nach **Bern-Belp (LSZB)**
Callsign: **HB-LUK** (Hotel Bravo Lima Uniform Kilo)
Strecke: ca. 18 NM, Heading ~110° (ESE)

---

## Voraussetzungen

- Flugzeug am Gate/Parking in LSZG, Avionics ON
- COM1 auf LSZG Ground-Frequenz (121.800 MHz)
- PTT-Taste konfiguriert
- API Key gespeichert
- Karte: VFR-Streckenführung Grenchen → Bern (z.B. via Lyss / Aarberg)

## Frequenzen

### LSZG Grenchen

| Service | Frequenz |
|---------|----------|
| ATIS    | 121.100  |
| Ground  | 121.800  |
| Tower   | 120.100  |

### LSZB Bern-Belp

| Service  | Frequenz |
|----------|----------|
| ATIS     | 125.130  |
| Delivery | 121.960  |
| Tower    | 121.025  |
| Arrival  | 127.325  |

*Hinweis: LSZB hat keine separate Ground-Frequenz — Tower handled auch Taxi (der Plugin erkennt das via `tower_only=1`).*

*Tipp: Im ATC Commands Panel klick auf eine Frequenz → wird in COM Standby geladen, dann mit ← → aktivieren.*

---

## Übersicht des Flows

```
LSZG: IDLE → TAXI_CLEARED → TOWER_CONTACT → DEPARTURE_CLEARED (cross-country)
        ↓
   LEAVING_FREQUENCY
        ↓
   EN_ROUTE  (kein ATC-Kontakt, Funk frei)
        ↓
   nearest airport wechselt LSZG → LSZB → State-Reset zu IDLE
        ↓
LSZB: IDLE → INITIAL_CALL_INBOUND → PATTERN_ENTRY → LANDING_CLEARED → IDLE
```

**Wichtig:** Damit ATC den Abflug als Cross-Country erkennt (und keine Platzrundenfreigabe gibt), muss der Pilot beim *ready for departure* seine Absicht ankündigen — Stichwort **`on course`** (alternativ `northbound/southbound/eastbound/westbound departure`, `VFR to LSZB`, `cross country`). Ohne diesen Zusatz interpretiert der Tower den Abflug als Platzrunde.

---

## Phase 1 — Abflug Grenchen (LSZG)

### 1. Ground kontaktieren + Taxi anfordern

*COM1 auf 121.800 MHz (LSZG Ground)*

**Pilot:**
> "Grenchen Ground, Hotel Bravo Lima Uniform Kilo, request taxi"

**Erwartete ATC-Antwort:**
> "Hotel Bravo Lima Uniform Kilo, Grenchen Ground, information Alpha, taxi to holding point runway 06 via Alpha, QNH 1013."

**Pilot-Antwort: READBACK**
> "Taxi to holding point runway 06 via Alpha, QNH 1013, Hotel Bravo Lima Uniform Kilo"

**ATC:** Stille → bei Holding Point auf Tower wechseln

---

### 2. Tower kontaktieren + Cross-Country Departure Request

*COM1 auf 120.100 MHz (LSZG Tower)*

**Pilot:**
> "Grenchen Tower, Hotel Bravo Lima Uniform Kilo, holding short runway zero six, ready for departure, on course"

**Erwartete ATC-Antwort:**
> "Hotel Bravo Lima Uniform Kilo, Grenchen Tower, runway 06, cleared for takeoff, wind calm, on course approved, frequency change approved when airborne."

**Pilot-Antwort: READBACK**
> "Cleared for takeoff runway 06, on course, Hotel Bravo Lima Uniform Kilo"

**ATC:** Stille (korrekter Readback)

**State:** `TOWER_CONTACT → DEPARTURE_CLEARED` (departure_type = CROSS_COUNTRY)

**Log-Check:**
```
[xp_wellys_atc][DEBUG] Intent: READY_FOR_DEPARTURE_VFR (confidence=0.92)
[xp_wellys_atc] ATC state: TOWER_CONTACT -> DEPARTURE_CLEARED
[xp_wellys_atc] Departure type: CROSS_COUNTRY
```

---

### 3. Abheben + Frequenz verlassen (DEPARTURE_CLEARED → EN_ROUTE)

*Take-off, steigen auf VFR-Reiseflughöhe (z.B. 3500 ft MSL), Heading ESE.*

**Wichtig:** Der State bleibt `DEPARTURE_CLEARED`, bis der Pilot explizit die Frequenz verlässt. Die Pattern-Auto-Correction wird durch das Cross-Country-Flag unterdrückt.

**Log-Check (sollte auftauchen, sobald Phase auf PATTERN/CLIMB wechselt):**
```
[xp_wellys_atc] Skipping pattern auto-correction: cross-country departure
```

Sobald in der Luft (oder beim Verlassen der Kontrollzone):

**Pilot:**
> "Hotel Bravo Lima Uniform Kilo, leaving your frequency, good day"

**Erwartete ATC-Antwort:**
> "Hotel Bravo Lima Uniform Kilo, good day."

**State:** `DEPARTURE_CLEARED → EN_ROUTE`

*Alternativ als formelle Variante:*

**Pilot:**
> "Grenchen Tower, Hotel Bravo Lima Uniform Kilo, request frequency change"

**Erwartete ATC-Antwort:**
> "Hotel Bravo Lima Uniform Kilo, frequency change approved, good day."

---

## Phase 2 — En Route (EN_ROUTE)

*Strecke ca. 18 NM, ~10–12 Minuten Reiseflug.
COM1 kann auf 124.700 (FIS / Swiss Radar) oder UNICOM 122.800 gestellt werden — der Plugin reagiert in EN_ROUTE nicht auf PTT.*

### Verhalten in EN_ROUTE

| Aktion | Erwartet |
|---|---|
| PTT mit beliebigem Funkspruch | **Stille** (kein ATC-Kontakt) |
| Nächstgelegener Airport wechselt LSZG → LSZB | Log: `Airport changed: LSZG -> LSZB`, State-Reset zu `IDLE` |

**Log-Check (X-Plane Log.txt):**
```
[xp_wellys_atc] Airport changed: LSZG -> LSZB, resetting ATC state
[xp_wellys_atc] ATC state machine reset to IDLE
```

Sobald der State auf `IDLE` zurück ist und der nächste Airport LSZB ist, kann LSZB Tower kontaktiert werden.

---

## Phase 3 — Anflug Bern-Belp (LSZB)

### 4. (Optional) ATIS abhören

*COM1 auf 125.130 MHz (LSZB ATIS)*
ATIS spielt automatisch ab (sofern in Reichweite). Information notieren: Buchstabe, aktive Piste, Wind, QNH.

**Platzrundenhöhe LSZB:** ca. **2500 ft MSL** (~1000 ft über Platz, Platzhöhe 1675 ft)
**Aktive Pisten LSZB:** RWY 14 / RWY 32 (je nach Wind)

---

### 5. Tower kontaktieren — Inbound Call

*COM1 auf 121.025 MHz (LSZB Tower), ca. 8–10 NM vor LSZB*

**Pilot:**
> "Bern Tower, Hotel Bravo Lima Uniform Kilo, ten miles northwest, inbound for landing, information Bravo"

**Erwartete ATC-Antwort:**
> "Hotel Bravo Lima Uniform Kilo, Bern Tower, enter left downwind runway 14, report midfield downwind."

**Pilot-Antwort: READBACK**
> "Enter left downwind runway 14, wilco report midfield downwind, Hotel Bravo Lima Uniform Kilo"

**State:** `IDLE → PATTERN_ENTRY`

---

### 6. Downwind melden (PATTERN_ENTRY → PATTERN_ENTRY)

*Querab der Pistenmitte auf Platzrundenhöhe (~2500 ft MSL)*

**Pilot:**
> "Hotel Bravo Lima Uniform Kilo, midfield left downwind runway one four"

**Erwartete ATC-Antwort:**
> "Hotel Bravo Lima Uniform Kilo, number one, runway 14, continue approach, report final."

**Pilot-Antwort: WILCO**
> "Wilco, will report final, Hotel Bravo Lima Uniform Kilo"

---

### 7. Final melden (PATTERN_ENTRY → LANDING_CLEARED)

*Eingedreht auf Endanflug Heading ~140°*

**Pilot:**
> "Hotel Bravo Lima Uniform Kilo, final runway one four"

**Erwartete ATC-Antwort:**
> "Hotel Bravo Lima Uniform Kilo, runway 14, cleared to land, wind calm."

**Pilot-Antwort: READBACK**
> "Cleared to land runway 14, Hotel Bravo Lima Uniform Kilo"

---

### 8. Runway verlassen + Taxi to Parking (LANDING_CLEARED → IDLE)

*Landen, Runway via Taxiway verlassen. LSZB ist `tower_only` — kein Frequenzwechsel nötig, Tower erledigt auch Taxi.*

**Pilot:**
> "Bern Tower, Hotel Bravo Lima Uniform Kilo, clear of runway one four, request taxi to general aviation parking"

**Erwartete ATC-Antwort:**
> "Hotel Bravo Lima Uniform Kilo, taxi to general aviation parking via Alpha, good day."

**Pilot-Antwort:**
> "Taxi to parking via Alpha, Hotel Bravo Lima Uniform Kilo, good day"

---

## Verifikations-Checkliste

| # | Prüfpunkt | Erwartung |
|---|---|---|
| 1 | LSZG Ground → Taxi Clearance | State `TAXI_CLEARED` |
| 2 | LSZG Tower → "ready for departure, on course" | Intent `READY_FOR_DEPARTURE_VFR`, State `DEPARTURE_CLEARED`, Log `Departure type: CROSS_COUNTRY` |
| 3 | Nach Takeoff in PATTERN/CLIMB-Phase | Log `Skipping pattern auto-correction`, State bleibt `DEPARTURE_CLEARED` |
| 4 | "leaving your frequency, good day" | State `EN_ROUTE`, ATC sagt "good day." |
| 5 | EN_ROUTE: PTT mit beliebigem Spruch | Stille, kein ATC-Output |
| 6 | LSZG → LSZB Wechsel des nächstgelegenen Airports | Log `Airport changed: LSZG -> LSZB`, State `IDLE` |
| 7 | LSZB Inbound Call | State `PATTERN_ENTRY`, Pattern-Direction-Anweisung |
| 8 | LSZB Downwind/Final/Landing | Standard-Landing-Flow |
| 9 | LSZB Runway vacated | State `IDLE`, Handoff zu Ground |

---

## Häufige Stolperfallen

1. **`ready for departure` ohne `on course`** — wird als Platzrunden-Abflug interpretiert. ATC sagt "report left downwind", State geht nach Takeoff automatisch nach `PATTERN_ENTRY`. Dann funktioniert `leaving frequency` nicht mehr (kein Template in PATTERN_ENTRY).
2. **Zu früh `leaving frequency`** — wenn noch nicht abgehoben (FlightPhase ≠ CLIMB/PATTERN/CRUISE), wird der Intent durch die Phase-Precondition abgewiesen mit "you are still on the ground."
3. **`request frequency change` aus `IDLE`** — kein Effekt, da es im IDLE-Template kein REQUEST_FREQUENCY gibt.
4. **Nearest-Airport-Wechsel zu früh** — wenn LSZB schon vor dem `leaving frequency` nächstliegender Airport ist, springt der State nicht (Wechsel-Detection läuft nur in EN_ROUTE).

---

## Log prüfen

In X-Plane `Log.txt` nach `[xp_wellys_atc]` suchen:

```
[xp_wellys_atc][DEBUG] Intent: READY_FOR_DEPARTURE_VFR (confidence=0.92)
[xp_wellys_atc] ATC state: TOWER_CONTACT -> DEPARTURE_CLEARED
[xp_wellys_atc] Departure type: CROSS_COUNTRY
[xp_wellys_atc] Skipping pattern auto-correction: cross-country departure
[xp_wellys_atc] ATC state: DEPARTURE_CLEARED -> EN_ROUTE
[xp_wellys_atc] Airport changed: LSZG -> LSZB, resetting ATC state
[xp_wellys_atc] ATC state machine reset to IDLE
[xp_wellys_atc] ATC state: IDLE -> PATTERN_ENTRY
[xp_wellys_atc] ATC state: PATTERN_ENTRY -> LANDING_CLEARED
```
