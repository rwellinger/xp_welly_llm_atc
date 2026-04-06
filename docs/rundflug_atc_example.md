# Rundflug (Traffic Pattern) — ATC Test Script

Towered Airport, z.B. **LSZG Grenchen**
Callsign: **HB-LUK** (Hotel Bravo Lima Uniform Kilo)

---

## Voraussetzungen

- Flugzeug am Gate/Parking, Avionics ON
- COM1 auf Ground-Frequenz (LSZG: 121.800 MHz)
- PTT-Taste konfiguriert
- API Key gespeichert

## LSZG Frequenzen

| Service | Frequenz |
|---------|----------|
| ATIS    | 121.100  |
| Ground  | 121.800  |
| Tower   | 120.100  |

*Tipp: Im ATC Commands Panel klick auf eine Frequenz → wird in COM Standby geladen, dann mit ← → aktivieren.*

---

## Platzrunde (Traffic Pattern) bei LSZG

```
                    Downwind (gegenläufig zur Piste)
          ←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←←
          ↓         ▼ hier melden              ↑
          ↓      "midfield downwind"           ↑  Crosswind
   Base   ↓                                    ↑
          ↓                                    ↑
          →→→→→→→→→→→→→→→→→→→→→→→→→→→→→→→→→→→→→
   Final     ======= RWY 06 ========>     Takeoff
```

**Platzrundenhöhe LSZG:** ca. **2400 ft MSL** (~1000 ft über Platz, Platzhöhe 1411 ft)

### Wann was melden:

1. **Start** → steigen auf Platzrundenhöhe
2. **Crosswind** → 90° Links-Kurve (bei left traffic)
3. **Downwind** → parallel zur Piste, **Gegenrichtung** (du fliegst Heading ~240° bei RWY 06)
4. **Melden bei "midfield downwind"** → wenn du **querab der Pistenmitte** bist, auf Platzrundenhöhe (~2400 ft MSL)
5. **Base** → 90° Links-Kurve Richtung Piste
6. **Final** → eingedreht auf Pistenkurs, melden auf Final

---

## Ablauf

### 1a. Ground kontaktieren + Taxi anfordern (IDLE → TAXI_CLEARED, kombiniert)

*In der Praxis kombiniert man Initial Call + Taxi Request in einer Transmission.*
*COM1 auf Ground-Frequenz (121.800 MHz)*

**Pilot:**
> "Grenchen Ground, Hotel Bravo Lima Uniform Kilo, request taxi"

**Erwartete ATC-Antwort:**
> "Hotel Bravo Lima Uniform Kilo, Grenchen Ground, information Alpha, taxi to holding point runway 06 via Alpha, QNH 1013."

### 1b. Alternativ: Zwei-Schritt-Flow (IDLE → GROUND_CONTACT → TAXI_CLEARED)

**Pilot:**
> "Grenchen Ground, Hotel Bravo Lima Uniform Kilo"

**Erwartete ATC-Antwort:**
> "Hotel Bravo Lima Uniform Kilo, Grenchen Ground, information Alpha, runway 06, QNH 1013."

---

### 2. Taxi anfordern (GROUND_CONTACT → TAXI_CLEARED, nur bei 1b)

**Pilot:**
> "Request taxi, Hotel Bravo Lima Uniform Kilo"

**Erwartete ATC-Antwort:**
> "Hotel Bravo Lima Uniform Kilo, taxi to holding point runway 06 via Alpha, QNH 1013."

---

### 3. Readback Taxi-Clearance (bleibt TAXI_CLEARED)

**Pilot:**
> "Taxi to holding point runway zero six via Alpha, QNH one zero one three, Hotel Bravo Lima Uniform Kilo"

**Erwartete ATC-Antwort:**
> "Hotel Bravo Lima Uniform Kilo, readback correct, contact tower when ready."

---

### 4. Tower kontaktieren (TAXI_CLEARED → TOWER_CONTACT)

*COM1 auf Tower-Frequenz wechseln (120.100 MHz)*

**Pilot:**
> "Grenchen Tower, Hotel Bravo Lima Uniform Kilo, holding short runway zero six"

**Erwartete ATC-Antwort:**
> "Hotel Bravo Lima Uniform Kilo, Grenchen Tower, runway 06, hold short, number one."

---

### 5. Ready for Departure (TOWER_CONTACT → DEPARTURE_CLEARED)

**Pilot:**
> "Hotel Bravo Lima Uniform Kilo, ready for departure"

**Erwartete ATC-Antwort:**
> "Hotel Bravo Lima Uniform Kilo, runway 06, cleared for takeoff, wind calm."

---

### 6. Readback Takeoff-Clearance (DEPARTURE_CLEARED → PATTERN_ENTRY)

**Pilot:**
> "Cleared for takeoff runway zero six, Hotel Bravo Lima Uniform Kilo"

**Erwartete ATC-Antwort:**
> "Hotel Bravo Lima Uniform Kilo, readback correct, report downwind runway 06."

---

*Abheben, Platzrunde fliegen (Crosswind → Downwind → Base → Final)*
*Auf Platzrundenhöhe steigen (~2400 ft MSL)*

---

### 7. Downwind melden (PATTERN_ENTRY → PATTERN_ENTRY)

*Querab der Pistenmitte, auf Platzrundenhöhe, Heading ~240°*

**Pilot:**
> "Grenchen Tower, Hotel Bravo Lima Uniform Kilo, midfield left downwind runway zero six"

**Erwartete ATC-Antwort:**
> "Hotel Bravo Lima Uniform Kilo, number one, runway 06, continue approach, report final."

---

### 8. Final melden (PATTERN_ENTRY → LANDING_CLEARED)

*Eingedreht auf Endanflug Heading ~060°*

**Pilot:**
> "Hotel Bravo Lima Uniform Kilo, final runway zero six"

**Erwartete ATC-Antwort:**
> "Hotel Bravo Lima Uniform Kilo, runway 06, cleared to land, wind calm."

---

### 9. Runway verlassen (LANDING_CLEARED → IDLE)

*Landen, Runway via Taxiway verlassen, noch auf Tower-Frequenz!*

**Pilot:**
> "Grenchen Tower, Hotel Bravo Lima Uniform Kilo, clear of runway zero six"

**Erwartete ATC-Antwort:**
> "Hotel Bravo Lima Uniform Kilo, contact ground on 121.800, good day."

---

### 10. Ground kontaktieren + Taxi to Parking (IDLE → IDLE)

*COM1 auf Ground-Frequenz wechseln (121.800 MHz)*

**Pilot:**
> "Grenchen Ground, Hotel Bravo Lima Uniform Kilo, request taxi to general aviation parking"

**Erwartete ATC-Antwort:**
> "Hotel Bravo Lima Uniform Kilo, taxi to general aviation parking via Alpha, good day."

---

## Edge Cases zum Testen

### Radio Check (IDLE, beliebiger State)

**Pilot:**
> "Radio check"

**Erwartet:**
> "Hotel Bravo Lima Uniform Kilo, Grenchen Tower, reading you five by five."

### Unverständliche Eingabe (_INVALID)

**Pilot:**
> "Ich möchte eine Pizza bestellen"

**Erwartet (je nach State):**
> "Hotel Bravo Lima Uniform Kilo, say again your request."

### Ready for Departure direkt aus IDLE (auf Tower-Freq)

**Pilot:**
> "Ready for departure"

**Erwartet (wenn auf Tower-Frequenz):**
> "Hotel Bravo Lima Uniform Kilo, Grenchen Tower, runway 06, cleared for takeoff, wind calm."

*Funktioniert als kombinierter Initial Call + Departure Request.*

### REQUEST_TAXI auf falscher Frequenz (Tower statt Ground)

**Pilot (auf Tower-Freq):**
> "Request taxi"

**Erwartet:**
> "Hotel Bravo Lima Uniform Kilo, contact ground for taxi."

---

## Log prüfen

In X-Plane `Log.txt` nach `[xp_wellys_atc]` suchen:

```
[xp_wellys_atc][DEBUG] Whisper response: "..."     ← Transkription korrekt?
[xp_wellys_atc][DEBUG] Intent: ... (confidence=...) ← Intent + Confidence
[xp_wellys_atc] ATC state: IDLE -> GROUND_CONTACT  ← State-Transition
[xp_wellys_atc] Routing to GPT intent classification ← GPT-Fallback aktiv?
[xp_wellys_atc][DEBUG] COM1: 120.100 MHz -> Tower  ← Frequenz-Erkennung
```
