# xp_wellys_atc — Benutzerhandbuch

## 1. Einleitung

**xp_wellys_atc** ist ein KI-gestütztes ATC-Sprachkommunikations-Plugin für X-Plane 12 auf macOS. Es ermöglicht realistische VFR-Funkkommunikation mit einer virtuellen Flugsicherung per Spracheingabe.

### Funktionsweise

```
Push-to-Talk → Mikrofonaufnahme → OpenAI Whisper (Spracherkennung)
    → Intent-Parser (regelbasiert + GPT-Fallback) → ATC-Zustandsautomat
    → Template-Antwort → OpenAI TTS (Sprachsynthese) → Audiowiedergabe
```

1. **PTT drücken** — das Plugin nimmt die Mikrofoneingabe auf
2. **Spracherkennung** — OpenAI Whisper transkribiert den Funkspruch
3. **Intent-Klassifikation** — ein regelbasierter Parser erkennt die Absicht (z.B. "request taxi", "ready for departure"). Bei niedriger Konfidenz klassifiziert GPT-4o-mini als Fallback
4. **ATC-Zustandsautomat** — validiert die Absicht gegen den aktuellen Gesprächszustand und die Flugphase, generiert dann eine passende ATC-Antwort aus Templates
5. **Sprachwiedergabe** — OpenAI TTS wandelt die Antwort in Sprache um und gibt sie wieder

Das Plugin unterstützt **kontrollierte Flugplätze** (vollständiger Ground/Tower-Ablauf) und **unkontrollierte Flugplätze** (CTAF Self-Announce). Zusätzlich werden **ATIS-Meldungen** aus Live-Wetterdaten generiert, wenn die ATIS-Frequenz eingestellt wird.

---

## 2. Konfiguration

### 2.1 API-Schlüssel

Das Plugin benötigt einen OpenAI-API-Schlüssel. Dieser wird ausschliesslich im **macOS Keychain** gespeichert — er wird niemals in eine Datei geschrieben.

Einrichtung:
1. X-Plane starten und das Plugin-Einstellungsfenster öffnen
2. OpenAI-API-Schlüssel im Einstellungspanel eingeben
3. Der Schlüssel wird im Keychain gespeichert; `settings.json` enthält nur `"api_key_saved": true`

### 2.2 Einstellungsdatei (`data/settings.json`)

| Einstellung | Typ | Standard | Beschreibung |
|---|---|---|---|
| `api_key_saved` | bool | `false` | Flag, ob ein API-Schlüssel im Keychain gespeichert ist |
| `ptt_key_vk` | int | `49` | Virtueller Tastencode für Push-to-Talk (Tastatur) |
| `ptt_joystick_button` | int | `-1` | Joystick-Button-Index für PTT (`-1` = deaktiviert) |
| `pilot_callsign_raw` | string | `"N123AB"` | Luftfahrzeugkennung (Rohformat) |
| `pilot_callsign` | string | `"November One Two Three Alpha Bravo"` | ICAO-phonetisches Rufzeichen (automatisch generiert) |
| `active_com` | int | `1` | Überwachtes COM-Radio (`1` oder `2`) |
| `volume` | float | `1.0` | Wiedergabelautstärke der ATC-Antworten (`0.0`–`1.0`) |
| `pattern_direction` | string | `"left"` | Standard-Platzrundenrichtung (wird pro Flugplatz/Piste durch `airport_vrps.json` überschrieben) |
| `tts_voice_tower` | string | `"onyx"` | OpenAI-TTS-Stimme für Tower-Antworten |
| `tts_voice_ground` | string | `"echo"` | OpenAI-TTS-Stimme für Ground-Antworten |
| `tts_voice_atis` | string | `"nova"` | OpenAI-TTS-Stimme für ATIS-Meldungen |
| `tts_model` | string | `"tts-1"` | OpenAI-TTS-Modell |
| `whisper_model` | string | `"whisper-1"` | OpenAI-Spracherkennungsmodell |
| `gpt_model` | string | `"gpt-4o-mini"` | GPT-Modell für Intent-Klassifikations-Fallback |
| `gpt_fallback_enabled` | bool | `true` | GPT-Fallback aktivieren bei niedriger Parser-Konfidenz |
| `disable_default_atc` | bool | `false` | Standard-X-Plane-ATC-Meldungen unterdrücken |
| `skip_radio_power_check` | bool | `false` | Funkstrom-Prüfung umgehen (Workaround für exotische Flugzeuge) |
| `show_phraseology_hints` | bool | `true` | Phraseologie-Spickzettel im ATC-Panel anzeigen |
| `auto_correction_factor` | float | `1.0` | Multiplikator für ATC-Recovery-Timeout (`0.5`--`2.0`). Niedrig = schnellere Korrektur, hoch = mehr Zeit zum Funken |
| `debug_logging` | bool | `false` | Ausführliche Debug-Ausgabe in X-Plane Log.txt aktivieren |

### 2.3 Push-to-Talk-Zuweisung

PTT kann über das X-Plane-Command-System zugewiesen werden:

- **X-Plane Command:** `xp_wellys_atc/ptt`
- Dieses Command in den X-Plane Tastatur-/Joystick-Einstellungen einer beliebigen Taste oder einem Joystick-Button zuweisen
- Die Einstellungen `ptt_key_vk` und `ptt_joystick_button` in `settings.json` bieten eine alternative Direktzuweisung

### 2.4 COM-Radio-Auswahl

Das Plugin überwacht das durch `active_com` (1 oder 2) festgelegte COM-Radio. Es gleicht die aktive COM-Frequenz mit der Frequenzdatenbank des nächstgelegenen Flugplatzes ab (aus X-Planes `apt.dat` geparst), um zu bestimmen, ob Ground, Tower, ATIS oder UNICOM eingestellt ist.

---

## 3. Datendateien — Referenz

Alle Datendateien befinden sich im Verzeichnis `data/` innerhalb des Plugin-Ordners.

### 3.1 `atc_templates.json` — ATC-Antwort-Templates

Definiert alle ATC-Antworttexte in hierarchischer Struktur:

```
Flugplatztyp → ATC-Zustand → Pilot-Intent → Antwort
```

**Struktur:**
- **`towered`** — Antworten für kontrollierte Flugplätze (Ground + Tower)
- **`uncontrolled`** — Antworten für CTAF/UNICOM-Flugplätze

Jeder Antworteintrag enthält:

| Feld | Beschreibung |
|---|---|
| `response` | Template-Text mit `{Variable}`-Platzhaltern |
| `next_state` | Zustandsübergang nach dieser Antwort |
| `requires_readback` | Ob der Pilot die Freigabe zurücklesen muss |

**Template-Variablen** (werden zur Laufzeit aus X-Plane-Daten befüllt):

| Variable | Quelle |
|---|---|
| `{callsign}` | Pilot-Rufzeichen aus den Einstellungen |
| `{airport}` | Name des nächsten Flugplatzes |
| `{runway}` | Windbestimmte aktive Piste |
| `{wind}` | Aktuelle Windrichtung und -stärke |
| `{qnh}` | Luftdruck in hPa |
| `{atis_letter}` | Aktueller ATIS-Informationsbuchstabe (Alpha–Zulu) |
| `{pattern_direction}` | Platzrundenseite (left/right) |
| `{entry_vrp}` | Erkannter Visual Reporting Point |
| `{frequency}` | Ground-/Übergabe-Frequenz |
| `{position_remark}` | Positionsbeschreibung |

Der Schlüssel `_INVALID` in jedem Zustand ist die Fallback-Antwort, wenn kein passender Intent gefunden wird (typischerweise eine "say again"-Antwort).

### 3.2 `flight_rules.json` — Flugphasen-Regeln und Schutzmechanismen

Steuert, wie das Plugin die Flugphase erkennt und ungültige Funksprüche verhindert.

**Phasenschwellwerte:**

| Parameter | Wert | Zweck |
|---|---|---|
| `taxi_min_gs_kt` | 5.0 | Minimale Geschwindigkeit über Grund für TAXI-Phase |
| `roll_min_gs_kt` | 40.0 | Minimale Geschwindigkeit für TAKEOFF_ROLL |
| `climb_min_vs_fpm` | 300.0 | Minimale Vertikalgeschwindigkeit für CLIMB |
| `pattern_max_agl_ft` | 3000.0 | Maximale AGL-Höhe für PATTERN |
| `near_airport_nm` | 5.0 | Maximale Distanz für "nahe am Flugplatz" |
| `runway_aligned_deg` | 30.0 | Kurskurstoleranz für Pistenanflug |
| `final_descent_rate_fpm` | -200.0 | Minimale Sinkrate für FINAL_APPROACH |

**Hysterese** (Anti-Jitter-Verzögerungen):

| Parameter | Wert | Zweck |
|---|---|---|
| `ground_to_airborne_sec` | 0.5 | Verzögerung vor Übergang zu "in der Luft" |
| `airborne_to_landing_sec` | 0.3 | Verzögerung vor Übergang zu "gelandet" |
| `auto_correction_delay_sec` | 3.0 | Standardverzögerung für automatische Zustandskorrekturen |

**Intent-Vorbedingungen:**
Das Plugin blockiert ungültige Funksprüche basierend auf der aktuellen Flugphase. Beispiele:
- Taxi-Anfrage ist nicht möglich, wenn man in der Luft ist
- "Runway vacated" ist nicht möglich, wenn man noch fliegt
- "Inbound"-Meldung ist nicht möglich, wenn man am Boden steht

Bei einem ungültigen Funkspruch antwortet ATC mit einer Ablehnungsmeldung (z.B. *"Unable, you appear to be airborne."*).

**Auto-Korrekturen:**
Das Plugin korrigiert Zustands-/Phasen-Abweichungen automatisch nach einer konfigurierbaren Verzögerung. Beispiele:
- In der Luft, aber Zustand noch `DEPARTURE_CLEARED` → automatischer Übergang zu `PATTERN_ENTRY` nach 5 Sekunden
- Am Boden, aber Zustand noch `PATTERN_ENTRY` → automatischer Reset zu `IDLE` nach 3 Sekunden

**Frequenzeinschränkungen:**
Bestimmte Intents sind nur auf bestimmten Frequenzen gültig:
- `REQUEST_TAXI` — nur auf Ground-Frequenz
- `READY_FOR_DEPARTURE` — auf Tower- oder Ground-Frequenz (am Holding Point meldet der Pilot "ready for departure" auf Ground, was einen Tower-Handoff auslöst)
- `SELF_ANNOUNCE` — nur auf UNICOM/CTAF

### 3.3 `airport_vrps.json` — Visual Reporting Points

Definiert Visual Reporting Points (VRPs) und Platzrundenrichtungen für bestimmte Flugplätze.

**Struktur pro Flugplatz:**

| Feld | Beschreibung |
|---|---|
| `name` | Flugplatzname |
| `pattern_direction` | Links/rechts pro Piste (oder global für alle Pisten) |
| `vrps` | Liste der VRPs mit Name, Breitengrad, Längengrad, Höhe |
| `arrival_routes` | Empfohlene VRP-Abfolgen pro Piste |

**Unterstützte Flugplätze:**

| ICAO | Name | Land |
|---|---|---|
| LSZB | Bern-Belp | Schweiz |
| LSZG | Grenchen | Schweiz |
| LSZO | Birrfeld | Schweiz |
| LSZR | St. Gallen-Altenrhein | Schweiz |
| LSZC | Buochs | Schweiz |
| LSGS | Sion | Schweiz |
| EDFE | Egelsbach | Deutschland |
| EDKB | Bonn-Hangelar | Deutschland |
| EDMA | Augsburg | Deutschland |
| EDTF | Freiburg | Deutschland |
| EDNY | Friedrichshafen | Deutschland |

**Verwendung:** Wenn die eigene Position über einem VRP gemeldet wird (z.B. *"Bern Tower, over Whiskey, inbound"*), erkennt das Plugin den VRP-Namen und ATC antwortet mit Einflug-Anweisungen über diesen Punkt.

Flugplätze, die nicht in dieser Datei aufgeführt sind, verwenden die globale `pattern_direction` aus den Einstellungen und haben keine VRP-Erkennung.

### 3.4 `atc_prompt_templates.json` — OpenAI-API-Prompts

Enthält die Prompts, die an die OpenAI-APIs gesendet werden:

| Prompt | Zweck |
|---|---|
| `whisper_prompt` | Statischer Kontext für Whisper zur Verbesserung der Erkennung von Flugfunkvokabular (NATO-Buchstabieralphabet, ATC-Phrasen) |
| `gpt_classify_prompt` | System-Prompt für GPT-Intent-Klassifikation, wenn die lokale Parser-Konfidenz unter 0.7 liegt. Enthält Flugkontext-Variablen |
| `gpt_fallback_prompt` | Notfall-Fallback-Prompt für vollständige ATC-Antwortgenerierung, wenn sowohl Parser als auch Klassifikator versagen |

Diese Prompts sind vorkonfiguriert und müssen in der Regel nicht angepasst werden.

---

## 4. ATC-Kommunikationsreferenz

### 4.1 Zustandsautomat — Übersicht

```
IDLE ──────────────────────────────────────────────────────┐
 ├── Ground kontaktieren ──→ GROUND_CONTACT ──→ TAXI_CLEARED
 ├── Tower kontaktieren ──→ TOWER_CONTACT ─────────────────┤
 └── Inbound-Meldung ──→ PATTERN_ENTRY ───────────────────┤
                                                           │
TOWER_CONTACT ─┬── Ready for Departure ──→ DEPARTURE_CLEARED
               ├── Request Landing ──→ PATTERN_ENTRY
               └── Request Touch & Go ──→ TOUCH_AND_GO_CLEARED
                                                           │
DEPARTURE_CLEARED ─┬── Report Downwind ──→ PATTERN_ENTRY   │
                   └── Leave Frequency ──→ EN_ROUTE ──→ IDLE
                                                           │
PATTERN_ENTRY ─┬── Report Final ──→ LANDING_CLEARED        │
               ├── Request Touch & Go ──→ TOUCH_AND_GO_CLEARED
               └── Go Around ──→ PATTERN_ENTRY             │
                                                           │
LANDING_CLEARED ─┬── Runway Vacated ──→ IDLE               │
                 └── Go Around ──→ PATTERN_ENTRY           │
                                                           │
TOUCH_AND_GO_CLEARED ─┬── Report Downwind ──→ PATTERN_ENTRY
                      ├── Runway Vacated ──→ IDLE
                      └── Go Around ──→ PATTERN_ENTRY
```

### 4.2 Zustände und gültige Intents

#### Zustand: `IDLE`

Ausgangszustand — kein aktives ATC-Gespräch.

| Pilot-Intent | Beispiel Funkspruch | ATC-Antwort |
|---|---|---|
| `INITIAL_CALL_GROUND` | *"Springfield Ground, N123AB, request taxi."* | *"N123AB, Springfield Ground, information Bravo current, runway 26, QNH 1013."* |
| `INITIAL_CALL_TOWER` | *"Springfield Tower, N123AB."* | *"N123AB, Springfield Tower, go ahead."* |
| `INITIAL_CALL_INBOUND` | *"Springfield Tower, N123AB, ten miles south, inbound for landing."* | *"N123AB, Springfield Tower, enter left downwind runway 26, report midfield downwind."* |
| `INITIAL_CALL_INBOUND_VRP` | *"Bern Tower, N123AB, over Whiskey, inbound."* | *"N123AB, Bern Tower, cleared to enter control zone via Whiskey, runway 14, report left downwind."* |
| `REQUEST_TAXI` | *"Springfield Ground, N123AB, request taxi."* | *"N123AB, Springfield Ground, information Bravo current, taxi to holding point runway 26 via Alpha, QNH 1013."* |
| `READY_FOR_DEPARTURE` | *"Springfield Tower, N123AB, holding short runway 26, ready for departure."* | *"N123AB, Springfield Tower, runway 26, cleared for takeoff, wind 240 at 8, report left downwind."* |
| `READY_FOR_DEPARTURE_VFR` | *"Springfield Tower, N123AB, ready for departure, on course."* | *"N123AB, Springfield Tower, runway 26, cleared for takeoff, wind 240 at 8, on course approved, frequency change approved when airborne."* |
| `RADIO_CHECK` | *"Springfield Tower, N123AB, radio check."* | *"N123AB, Springfield Tower, reading you five by five."* |

**Tipp — Position beim Erstkontakt angeben:** Bei `INITIAL_CALL_GROUND` und `REQUEST_TAXI` aus `IDLE` prüft das Plugin, ob du deinen Standort genannt hast (z.B. *"on parking"*, *"on the apron"*, *"at stand 5"*, *"on taxiway Alpha"*). Fehlt die Position, fügt der Controller einen kurzen "say position"-Hinweis in die Clearance ein. Funke wie in der echten Fliegerei — *"wen du rufst, wer du bist, wo du bist, was du willst"* — und du bekommst eine saubere Antwort.

#### Zustand: `GROUND_CONTACT`

Nach Erstkontakt mit Ground.

| Pilot-Intent | Beispiel Funkspruch | ATC-Antwort |
|---|---|---|
| `REQUEST_TAXI` | *"N123AB, request taxi."* | *"N123AB, taxi to holding point runway 26 via Alpha, QNH 1013."* |
| `READBACK` | *"Taxi runway 26 via Alpha, N123AB."* | *"N123AB, readback correct."* |

#### Zustand: `TAXI_CLEARED`

Rollen zum Rollhalt. Ground behaelt die Kontrolle auf dem Rollfeld;
der Tower-Handoff erfolgt erst, wenn der Pilot am Rollhalt "ready for
departure" meldet — nicht als Teil des Taxi-Readbacks.

| Pilot-Intent | Beispiel Funkspruch | ATC-Antwort |
|---|---|---|
| `READY_FOR_DEPARTURE` | *"Springfield Ground, N123AB, holding short runway 26, ready for departure."* | *"N123AB, roger, contact Tower on 120.100."* (→ `TOWER_CONTACT`) |
| `READY_FOR_DEPARTURE_VFR` | *"Ground, N123AB, holding short runway 26, ready for departure, VFR northbound."* | *"N123AB, roger, contact Tower on 120.100."* (→ `TOWER_CONTACT`) |
| `INITIAL_CALL_TOWER` | *"Springfield Tower, N123AB."* | *"N123AB, Tower, runway 26, hold short, number one."* |
| `READBACK` | *"Taxi runway 26 via Alpha, N123AB."* | *(still)* |

#### Zustand: `TOWER_CONTACT`

Tower hat bestätigt, aber noch keine Freigabe erteilt.

| Pilot-Intent | Beispiel Funkspruch | ATC-Antwort |
|---|---|---|
| `READY_FOR_DEPARTURE` | *"N123AB, ready for departure runway 26."* | *"N123AB, runway 26, cleared for takeoff, wind 240 at 8, report left downwind."* |
| `READY_FOR_DEPARTURE_VFR` | *"N123AB, ready for departure, on course."* | *"N123AB, runway 26, cleared for takeoff, wind 240 at 8, on course approved, frequency change approved when airborne."* |
| `REQUEST_LANDING` | *"N123AB, request landing runway 26."* | *"N123AB, number one, runway 26, report final."* |
| `REQUEST_TOUCH_AND_GO` | *"N123AB, request touch and go."* | *"N123AB, runway 26, cleared touch and go, wind 240 at 8."* |
| `REPORT_POSITION` | *"N123AB, five miles south."* | *"N123AB, number one, runway 26, report final."* |
| `READBACK` | *"Cleared for takeoff 26, N123AB."* | *"N123AB, readback correct."* |

#### Zustand: `DEPARTURE_CLEARED`

In der Luft nach Startfreigabe.

| Pilot-Intent | Beispiel Funkspruch | ATC-Antwort |
|---|---|---|
| `REPORT_POSITION_DOWNWIND` | *"N123AB, midfield left downwind runway 26."* | *"N123AB, number one, runway 26, continue approach, report final."* |
| `REPORT_POSITION_BASE` | *"N123AB, turning left base runway 26."* | *"N123AB, number one, runway 26, continue approach."* |
| `REPORT_POSITION_FINAL` | *"N123AB, final runway 26."* | *"N123AB, runway 26, cleared to land, wind 240 at 8."* |
| `REQUEST_FREQUENCY` | *"Tower, N123AB, request frequency change."* | *"N123AB, frequency change approved, good day."* |
| `LEAVING_FREQUENCY` | *"N123AB, leaving frequency, good day."* | *"N123AB, good day."* |

#### Zustand: `PATTERN_ENTRY`

In der Platzrunde (nach Inbound-Freigabe oder Downwind-Meldung).

| Pilot-Intent | Beispiel Funkspruch | ATC-Antwort |
|---|---|---|
| `REPORT_POSITION_DOWNWIND` | *"N123AB, midfield left downwind runway 26."* | *"N123AB, number one, runway 26, continue approach, report final."* |
| `REPORT_POSITION_BASE` | *"N123AB, turning left base runway 26."* | *"N123AB, number one, runway 26, continue approach."* |
| `REPORT_POSITION_FINAL` | *"N123AB, final runway 26."* | *"N123AB, runway 26, cleared to land, wind 240 at 8."* |
| `REQUEST_LANDING` | *"N123AB, request landing runway 26."* | *"N123AB, runway 26, cleared to land, wind 240 at 8."* |
| `REQUEST_TOUCH_AND_GO` | *"N123AB, request touch and go."* | *"N123AB, runway 26, cleared touch and go, wind 240 at 8."* |
| `GO_AROUND` | *"N123AB, going around."* | *"N123AB, roger, fly runway heading, climb and maintain pattern altitude, re-enter left downwind runway 26."* |

#### Zustand: `TOUCH_AND_GO_CLEARED`

Nach Touch-and-Go-Freigabe.

| Pilot-Intent | Beispiel Funkspruch | ATC-Antwort |
|---|---|---|
| `REPORT_POSITION_DOWNWIND` | *"N123AB, midfield left downwind runway 26."* | *"N123AB, number one, runway 26, continue approach, report final."* |
| `REPORT_POSITION_BASE` | *"N123AB, turning left base runway 26."* | *"N123AB, number one, runway 26, continue approach."* |
| `REPORT_POSITION_FINAL` | *"N123AB, final runway 26."* | *"N123AB, runway 26, cleared to land, wind 240 at 8."* |
| `REQUEST_LANDING` | *"N123AB, request full stop."* | *"N123AB, runway 26, cleared to land, wind 240 at 8."* |
| `REQUEST_TOUCH_AND_GO` | *"N123AB, request another touch and go."* | *"N123AB, runway 26, cleared touch and go, wind 240 at 8."* |
| `GO_AROUND` | *"N123AB, going around."* | *"N123AB, roger, fly runway heading, re-enter left downwind runway 26."* |
| `RUNWAY_VACATED` | *"N123AB, clear of runway 26."* | *"N123AB, contact ground on 121.9, good day."* |

#### Zustand: `LANDING_CLEARED`

Landefreigabe erteilt — warten auf Aufsetzen und Verlassen der Piste.

| Pilot-Intent | Beispiel Funkspruch | ATC-Antwort |
|---|---|---|
| `RUNWAY_VACATED` | *"N123AB, clear of runway 26."* | *"N123AB, contact ground on 121.9, good day."* |
| `REQUEST_TAXI_PARKING` | *"Ground, N123AB, request taxi to parking."* | *"N123AB, Ground, taxi to general aviation parking via Alpha."* |
| `GO_AROUND` | *"N123AB, going around."* | *"N123AB, roger, fly runway heading, climb and maintain pattern altitude, re-enter left downwind runway 26."* |

**Hinweis — `REQUEST_TAXI_PARKING` ist nur nach der Landung gültig** (Flugphasen `TAXI` oder `LANDING_ROLL`). Ein Taxi-to-Parking Request während du noch am Parkplatz stehst (Flugphase `GROUND_READY`) wird abgewiesen — man kann nicht dahin rollen wo man schon steht.

#### Zustand: `EN_ROUTE`

Überlandflug — kein ATC-Kontakt. Der Zustand wird automatisch auf `IDLE` zurückgesetzt, wenn sich der nächstgelegene Flugplatz ändert.

### 4.3 Funkdisziplin

ATC achtet auf unangemessene Sprache auf der Frequenz. Echte Controller reagieren auf unprofessionellen Funkverkehr — der virtuelle tut das ebenfalls:

1. **Erster Verstoss** — ein höflicher Hinweis zur Funkdisziplin; die eigentliche Anfrage des Piloten wird trotzdem normal bearbeitet
2. **Wiederholter Verstoss** — eine deutliche *"last warning"* des Controllers; weitere Transmissions bleiben möglich, aber die Geduld des Controllers geht sichtlich zu Ende

Das Feature soll realistische, professionelle Funkkommunikation fördern — nicht jeden Ausrutscher bestrafen. Wer am Funk ruhig bleibt, dem bleibt auch der Controller gewogen.

---

## 5. Beispiel: Platzrunde

Flugplatz: **LSZG Grenchen**, Piste **06**, Linksplatzrunde
Rufzeichen: **Hotel Bravo Lima Uniform Kilo** (HB-LUK)

### Schritt 1 — Ground kontaktieren

> **Pilot:** Grenchen Ground, Hotel Bravo Lima Uniform Kilo, request taxi.
>
> **ATC:** Hotel Bravo Lima Uniform Kilo, Grenchen Ground, information Alpha, taxi to holding point runway zero six via Alpha, QNH 1013.
>
> **Pilot (Readback):** Taxi to holding point runway zero six via Alpha, QNH 1013, Hotel Bravo Lima Uniform Kilo.

### Schritt 2 — Abflugbereit melden (Ground-Handoff)

> **Pilot:** Grenchen Ground, Hotel Bravo Lima Uniform Kilo, holding short runway zero six, ready for departure.
>
> **ATC (Ground):** Hotel Bravo Lima Uniform Kilo, roger, contact Tower on 120.100.
>
> **Pilot:** Contact Tower on 120.100, Hotel Bravo Lima Uniform Kilo.

*(Pilot wechselt auf Tower-Frequenz.)*

### Schritt 3 — Startfreigabe

> **Pilot:** Grenchen Tower, Hotel Bravo Lima Uniform Kilo, holding short runway zero six, ready for departure.
>
> **ATC (Tower):** Hotel Bravo Lima Uniform Kilo, runway zero six, cleared for takeoff, wind calm, report left downwind.
>
> **Pilot (Readback):** Cleared for takeoff runway zero six, wilco report downwind, Hotel Bravo Lima Uniform Kilo.

### Schritt 4 — Downwind melden

> **Pilot:** Grenchen Tower, Hotel Bravo Lima Uniform Kilo, midfield left downwind runway zero six.
>
> **ATC:** Hotel Bravo Lima Uniform Kilo, number one, runway zero six, continue approach, report final.
>
> **Pilot:** Wilco, will report final, Hotel Bravo Lima Uniform Kilo.

### Schritt 5 — Final melden

> **Pilot:** Hotel Bravo Lima Uniform Kilo, final runway zero six.
>
> **ATC:** Hotel Bravo Lima Uniform Kilo, runway zero six, cleared to land, wind calm.
>
> **Pilot (Readback):** Cleared to land runway zero six, Hotel Bravo Lima Uniform Kilo.

### Schritt 6 — Piste verlassen

> **Pilot:** Grenchen Tower, Hotel Bravo Lima Uniform Kilo, clear of runway zero six.
>
> **ATC:** Hotel Bravo Lima Uniform Kilo, contact ground on 121.800, good day.
>
> **Pilot:** Ground on 121.800, Hotel Bravo Lima Uniform Kilo, good day.

### Schritt 7 — Zum Parkplatz rollen

> **Pilot:** Grenchen Ground, Hotel Bravo Lima Uniform Kilo, request taxi to general aviation parking.
>
> **ATC:** Hotel Bravo Lima Uniform Kilo, taxi to general aviation parking via Alpha, good day.
>
> **Pilot:** Taxi to parking via Alpha, Hotel Bravo Lima Uniform Kilo, good day.

---

## 6. Beispiel: Überlandflug

Route: **LSZG Grenchen → LSZB Bern-Belp**
Rufzeichen: **Hotel Bravo Lima Uniform Kilo** (HB-LUK)

### Phase 1 — Abflug (LSZG)

#### Schritt 1 — Ground kontaktieren

> **Pilot:** Grenchen Ground, Hotel Bravo Lima Uniform Kilo, request taxi.
>
> **ATC:** Hotel Bravo Lima Uniform Kilo, Grenchen Ground, information Alpha, taxi to holding point runway zero six via Alpha, QNH 1013.
>
> **Pilot (Readback):** Taxi to holding point runway zero six via Alpha, QNH 1013, Hotel Bravo Lima Uniform Kilo.

#### Schritt 2 — Abflugbereit melden (Ground-Handoff)

Der Schlüsselbegriff **"on course"** signalisiert ATC, dass es sich um einen Überlandflug handelt, nicht um eine Platzrunde.

> **Pilot:** Grenchen Ground, Hotel Bravo Lima Uniform Kilo, holding short runway zero six, ready for departure, on course.
>
> **ATC (Ground):** Hotel Bravo Lima Uniform Kilo, roger, contact Tower on 120.100.
>
> **Pilot:** Contact Tower on 120.100, Hotel Bravo Lima Uniform Kilo.

*(Pilot wechselt auf Tower-Frequenz.)*

#### Schritt 3 — Startfreigabe (On Course)

> **Pilot:** Grenchen Tower, Hotel Bravo Lima Uniform Kilo, holding short runway zero six, ready for departure, on course.
>
> **ATC (Tower):** Hotel Bravo Lima Uniform Kilo, Grenchen Tower, runway zero six, cleared for takeoff, wind calm, on course approved, frequency change approved when airborne.
>
> **Pilot (Readback):** Cleared for takeoff runway zero six, on course, Hotel Bravo Lima Uniform Kilo.

#### Schritt 4 — Frequenz verlassen

> **Pilot:** Hotel Bravo Lima Uniform Kilo, leaving your frequency, good day.
>
> **ATC:** Hotel Bravo Lima Uniform Kilo, good day.

### Phase 2 — Streckenflug

Kein ATC-Kontakt. Reiseflug zum Zielflugplatz. Der Plugin-Zustand ist `EN_ROUTE`.

### Phase 3 — Anflug (LSZB)

#### Schritt 5 — Inbound-Meldung über VRP

Bern-Belp hat Visual Reporting Points: **November**, **Sierra**, **Whiskey**, **Echo**. Die eigene Position über dem überquerten VRP melden.

> **Pilot:** Bern Tower, Hotel Bravo Lima Uniform Kilo, over Whiskey, 3500 feet, inbound for landing, information Bravo.
>
> **ATC:** Hotel Bravo Lima Uniform Kilo, Bern-Belp Tower, cleared to enter control zone via Whiskey, runway one four, report left downwind.
>
> **Pilot (Readback):** Cleared via Whiskey, runway one four, wilco report left downwind, Hotel Bravo Lima Uniform Kilo.

*Ohne VRP:*

> **Pilot:** Bern Tower, Hotel Bravo Lima Uniform Kilo, ten miles northwest, inbound for landing, information Bravo.
>
> **ATC:** Hotel Bravo Lima Uniform Kilo, Bern Tower, enter left downwind runway one four, report midfield downwind.

#### Schritt 6 — Downwind melden

> **Pilot:** Hotel Bravo Lima Uniform Kilo, midfield left downwind runway one four.
>
> **ATC:** Hotel Bravo Lima Uniform Kilo, number one, runway one four, continue approach, report final.

#### Schritt 7 — Final melden und Landung

> **Pilot:** Hotel Bravo Lima Uniform Kilo, final runway one four.
>
> **ATC:** Hotel Bravo Lima Uniform Kilo, runway one four, cleared to land, wind calm.
>
> **Pilot (Readback):** Cleared to land runway one four, Hotel Bravo Lima Uniform Kilo.

#### Schritt 8 — Piste verlassen

LSZB hat keine separate Ground-Frequenz — Tower übernimmt das Rollen.

> **Pilot:** Bern Tower, Hotel Bravo Lima Uniform Kilo, clear of runway one four, request taxi to general aviation parking.
>
> **ATC:** Hotel Bravo Lima Uniform Kilo, taxi to general aviation parking via Alpha, good day.
>
> **Pilot:** Taxi to parking via Alpha, Hotel Bravo Lima Uniform Kilo, good day.

---

## 7. ATC Panel UI

Das ATC Commands Panel bietet Frequenzverwaltung, Phraseologie-Hilfe und Transkript-Verlauf.

### 7.1 Frequenz-Buttons

Das Panel zeigt alle Frequenzen des nächsten Flughafens (ATIS, Ground, Tower, Approach). Die aktuell aktive COM-Frequenz wird grün hervorgehoben. Ein Klick auf einen Frequenz-Button setzt diese als **Standby-Frequenz** -- Flip-Flop am COM-Radio zum Aktivieren.

Wenn das COM-Radio keinen Strom hat (Triebwerke aus, Avionik-Bus tot), werden die Frequenz-Buttons deaktiviert und eine Warnung angezeigt. Dies kann über die Einstellung `skip_radio_power_check` umgangen werden, z.B. für Flugzeuge mit ungewöhnlichen Elektrik-Systemen.

### 7.2 Phraseologie-Hinweise

Wenn `show_phraseology_hints` aktiviert ist (Standard), zeigt das Panel kontextbezogene Funkspruch-Vorschläge unterhalb der Frequenzliste. Die Hinweise aktualisieren sich dynamisch basierend auf ATC-Zustand, Flugphase und eingestellter Frequenz.

- **Grüner Text** -- der vorgeschlagene Funkspruch mit kurzem Rufzeichen (z.B. HB-AKA)
- **Hover-Tooltip** -- die vollständige ICAO-Phraseologie mit phonetischem Rufzeichen (z.B. Hotel Bravo Alpha Kilo Alpha)
- Hinweise sind in Kategorien gruppiert: Ground Operations, Tower Operations, Pattern/Approach, General

Die Hinweise sind schreibgeschützt -- alle Kommunikation erfolgt per Sprache (Push-to-Talk). Die Hinweise dienen als Spickzettel.

**EU/ICAO VFR-Ablauf an kontrollierten Flugplätzen mit Ground-Frequenz:**
An Flugplätzen mit separater Ground-Frequenz führen die Hinweise durch den korrekten Ablauf: zuerst Ground kontaktieren, Rollfreigabe erhalten, "ready for departure" auf Ground melden, dann Tower für Startfreigabe kontaktieren. Wenn Sie auf Tower eingestellt sind aber Ground verwenden sollten, zeigt das Panel "Tune to Ground frequency first".

### 7.3 Disregard-Button

Wenn der ATC-Zustand nicht IDLE ist (d.h. ein aktives Gespräch läuft), erscheint ein **Disregard**-Button neben der "Phraseology Hints"-Überschrift. Ein Klick setzt das ATC-Gespräch auf IDLE zurück.

Verwenden Sie diesen Button, wenn Sie in einer Schleife feststecken (z.B. ATC sagt wiederholt "say again") oder das aktuelle Gespräch abbrechen möchten. Der Flug wird nicht beeinflusst -- nur der ATC-Dialog wird zurückgesetzt.

### 7.4 Umliegende Flugplätze

Der aufklappbare Abschnitt "Nearby Airports" listet Flugplätze im Umkreis von 40 NM, sortiert nach Entfernung. Klicken Sie auf einen Flugplatz, um ihn als aktiven Flugplatz zu fixieren und dessen wichtigste Frequenz (ATIS > Tower > UNICOM) als Standby einzustellen. "Unlock" kehrt zur automatischen Erkennung des nächsten Flugplatzes zurück.
