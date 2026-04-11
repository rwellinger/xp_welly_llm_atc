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

## Readback-Regeln

| Situation | Pilot sagt | Wann |
|-----------|------------|------|
| **Readback** (vollständig) | Clearance-Inhalt wiederholen + Callsign | Taxi-Clearance, Takeoff-Clearance, Landing-Clearance, Hold-Short, Frequenzwechsel |
| **Wilco** | "Wilco" + Callsign | Anweisungen die man ausführen wird ("report downwind", "report final") |
| **Roger** | "Roger" + Callsign | Reine Informationen (Traffic-Info "number one", Wetter, ATIS-Info) |
| **Kein Readback** | — | ATC antwortet nicht auf korrekten Readback (Stille = alles OK) |

**Faustregel:** Alles was mit "cleared", "hold short", "taxi to", oder einer Frequenz zu tun hat → **Readback**. Alles andere → **Roger/Wilco**.

---

## Ablauf

### 1a. Ground kontaktieren + Taxi anfordern (IDLE → TAXI_CLEARED, kombiniert)

*In der Praxis kombiniert man Initial Call + Taxi Request in einer Transmission.*
*COM1 auf Ground-Frequenz (121.800 MHz)*

**Pilot:**
> "Grenchen Ground, Hotel Bravo Lima Uniform Kilo, request taxi"

**Erwartete ATC-Antwort:**
> "Hotel Bravo Lima Uniform Kilo, Grenchen Ground, information Alpha, taxi to holding point runway 06 via Alpha, QNH 1013."

**Pilot-Antwort: READBACK** (Taxi-Clearance + QNH sind readback-pflichtig)
> "Taxi to holding point runway 06 via Alpha, QNH 1013, Hotel Bravo Lima Uniform Kilo"

**ATC:** Stille (readback war korrekt) → Pilot wechselt auf Tower wenn bereit

### 1b. Alternativ: Zwei-Schritt-Flow (IDLE → GROUND_CONTACT → TAXI_CLEARED)

**Pilot:**
> "Grenchen Ground, Hotel Bravo Lima Uniform Kilo"

**Erwartete ATC-Antwort:**
> "Hotel Bravo Lima Uniform Kilo, Grenchen Ground, information Alpha, runway 06, QNH 1013."

**Pilot-Antwort: ROGER** (reine Informationen, keine Clearance)
> "Roger, Hotel Bravo Lima Uniform Kilo" *(oder direkt weiter mit Taxi Request)*

---

### 2. Taxi anfordern (GROUND_CONTACT → TAXI_CLEARED, nur bei 1b)

**Pilot:**
> "Request taxi, Hotel Bravo Lima Uniform Kilo"

**Erwartete ATC-Antwort:**
> "Hotel Bravo Lima Uniform Kilo, taxi to holding point runway 06 via Alpha, QNH 1013."

**Pilot-Antwort: READBACK**
> "Taxi to holding point runway 06 via Alpha, QNH 1013, Hotel Bravo Lima Uniform Kilo"

**ATC:** Stille (oder im Sim: "readback correct, contact tower when ready")

---

### ~~3. Readback Taxi-Clearance~~ (entfällt bei 1a, nur bei 1b nötig — siehe oben)

---

### 4. Tower kontaktieren (TAXI_CLEARED → TOWER_CONTACT)

*COM1 auf Tower-Frequenz wechseln (120.100 MHz)*

**Pilot:**
> "Grenchen Tower, Hotel Bravo Lima Uniform Kilo, holding short runway zero six, ready for departure"

**Erwartete ATC-Antwort:**
> "Hotel Bravo Lima Uniform Kilo, runway 06, cleared for takeoff, wind calm, report left downwind."

**Pilot-Antwort: READBACK** (Takeoff-Clearance) + **WILCO** (report downwind)
> "Cleared for takeoff runway 06, wilco report downwind, Hotel Bravo Lima Uniform Kilo"

**ATC:** Stille (korrekter Readback → keine Antwort!)

*Hinweis: In der Praxis kombiniert man "holding short + ready for departure" oft in einer Transmission, besonders bei kleinen Plätzen wie LSZG.*

*Wichtig — Pattern vs. Cross-Country:* Ohne Zusatz wie `on course` interpretiert ATC den Abflug als Platzrunde und sagt "report left downwind". Für einen Überlandflug (cross-country) muss der Pilot stattdessen sagen `ready for departure, on course` (siehe `cross_country_lszg_lszb.md`).

### 4b. Alternativ: Zwei-Schritt-Flow (erst Tower Contact, dann Ready)

**Pilot:**
> "Grenchen Tower, Hotel Bravo Lima Uniform Kilo, holding short runway zero six"

**Erwartete ATC-Antwort:**
> "Hotel Bravo Lima Uniform Kilo, Grenchen Tower, runway 06, hold short, number one."

**Pilot-Antwort: READBACK** (hold short ist readback-pflichtig!)
> "Hold short runway 06, Hotel Bravo Lima Uniform Kilo"

---

### 5. Ready for Departure (TOWER_CONTACT → DEPARTURE_CLEARED, nur bei 4b)

**Pilot:**
> "Hotel Bravo Lima Uniform Kilo, ready for departure"

**Erwartete ATC-Antwort:**
> "Hotel Bravo Lima Uniform Kilo, runway 06, cleared for takeoff, wind calm, report left downwind."

**Pilot-Antwort: READBACK + WILCO**
> "Cleared for takeoff runway 06, wilco report downwind, Hotel Bravo Lima Uniform Kilo"

**ATC:** Stille

---

*Abheben, Platzrunde fliegen (Crosswind → Downwind → Base → Final)*
*Auf Platzrundenhöhe steigen (~2400 ft MSL)*

---

### 6. Downwind melden (PATTERN_ENTRY → PATTERN_ENTRY)

*Querab der Pistenmitte, auf Platzrundenhöhe, Heading ~240°*

**Pilot:**
> "Grenchen Tower, Hotel Bravo Lima Uniform Kilo, midfield left downwind runway zero six"

**Erwartete ATC-Antwort:**
> "Hotel Bravo Lima Uniform Kilo, number one, runway 06, continue approach, report final."

**Pilot-Antwort: ROGER/WILCO** (Traffic-Info + Anweisung, keine Clearance)
> "Roger, will report final, Hotel Bravo Lima Uniform Kilo"

**ATC:** Stille

---

### 7. Final melden (PATTERN_ENTRY → LANDING_CLEARED)

*Eingedreht auf Endanflug Heading ~060°*

**Pilot:**
> "Hotel Bravo Lima Uniform Kilo, final runway zero six"

**Erwartete ATC-Antwort:**
> "Hotel Bravo Lima Uniform Kilo, runway 06, cleared to land, wind calm."

**Pilot-Antwort: READBACK** (Landing-Clearance!)
> "Cleared to land runway 06, Hotel Bravo Lima Uniform Kilo"

**ATC:** Stille

---

### 8. Runway verlassen (LANDING_CLEARED → IDLE)

*Landen, Runway via Taxiway verlassen, noch auf Tower-Frequenz!*

**Pilot:**
> "Grenchen Tower, Hotel Bravo Lima Uniform Kilo, clear of runway zero six"

**Erwartete ATC-Antwort:**
> "Hotel Bravo Lima Uniform Kilo, contact ground on 121.800, good day."

**Pilot-Antwort: READBACK** (Frequenzwechsel ist readback-pflichtig)
> "Ground on 121.800, Hotel Bravo Lima Uniform Kilo, good day"

**ATC:** Stille → Pilot wechselt auf Ground

---

### 9. Ground kontaktieren + Taxi to Parking (IDLE → IDLE)

*COM1 auf Ground-Frequenz wechseln (121.800 MHz)*

**Pilot:**
> "Grenchen Ground, Hotel Bravo Lima Uniform Kilo, request taxi to general aviation parking"

**Erwartete ATC-Antwort:**
> "Hotel Bravo Lima Uniform Kilo, taxi to general aviation parking via Alpha, good day."

**Pilot-Antwort: ROGER** (einfache Taxi-Anweisung zum Parking, optional Readback)
> "Taxi to parking via Alpha, Hotel Bravo Lima Uniform Kilo, good day"

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
