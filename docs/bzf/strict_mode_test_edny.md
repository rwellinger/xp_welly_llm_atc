# BZF-Strict-Mode Test-Flug — EDNY Friedrichshafen

> **Was du hier testest:** Phase-2-MVP des Strict-Modes — der Tower
> prüft jedes Pilot-Readback gegen die NfL Sprechfunk 2024 §25 b) Nr. 1
> Pflicht-Liste (Pistennummer, QNH, Frequenz, Squawk, Rufzeichen).
> Fehlt etwas, kommt eine korrektive Tower-Antwort statt stiller
> Akzeptanz — und der State advanced **nicht**, bis du sauber
> zurückliest.
>
> **Status MVP:** Strict-Mode greift beim READBACK-Intent. Erstanruf-
> Pflichtelement-Check (z. B. „du hast den ATIS-Letter im Erstanruf
> vergessen") ist nicht Teil des MVP — siehe **Bekannte
> Beschränkungen** am Ende.
>
> **Letzte Änderung:** 2026-06-05 · siehe [`bzf_coverage.md`](bzf_coverage.md)

---

## Vorbereitung

### Plugin-Settings

1. **Profile** auf `DE/BZF` setzen
   (Settings-Tab → ATC-Profil → `DE/BZF`)
2. **BZF-Strict-Mode** einschalten
   (Settings-Tab → Checkbox „BZF-Strict-Mode (Tower prueft Pflicht-Readback)")
   → Toggle ist **nur sichtbar** wenn Profile=DE.
3. **Pilot-Callsign** auf D-Format setzen (z. B. `D-EXYZ`)
   (Settings-Tab → Pilot-Callsign → `D-EXYZ`)
   → Das Plugin expandiert das beim Sprechen zu `Delta Echo X-Ray Yankee Zulu`.
4. **Backend-Modus:** beliebig (Lokal oder OpenAI — Strict-Mode ist backend-agnostisch).

### X-Plane Setup

- **Aircraft:** Cessna 172 oder vergleichbare GA-Maschine
- **Flugplatz:** `EDNY` (Friedrichshafen)
- **Position:** GA-Vorfeld / Apron (Cold Start)
- **Engines:** aus (Plugin startet im `IDLE`, du kannst mit Funkprobe oder
  direkt mit Rollanmeldung beginnen)
- **Wetter:** beliebig — der Tower nutzt das aktuelle X-Plane-Wetter für
  Wind und QNH

### EDNY-Eckdaten (zur Orientierung)

| | |
|---|---|
| ATIS | 127.575 |
| Ground / Rollkontrolle | 121.825 |
| Tower / Turm | 120.075 |
| Pisten | 06 / 24 (beide left-traffic) |
| Pattern-Höhe | ca. 2500 ft MSL (≈ 1100 ft AGL) |
| VRPs | November (N), Oscar (NE), Sierra (S), Whiskey (W) |

---

## Flugverlauf — Platzrunde (Pattern Work)

Dieser Rundflug bleibt komplett in der Platzrunde von EDNY. Wenn du warm
bist und der Strict-Mode flüssig läuft, kannst du den optionalen
VRP-Abstecher am Ende dranhängen.

Pro Schritt steht:
- **Du:** was du auf PTT sagen sollst (NfL-konform)
- **Tower:** was du als Antwort erwartest
- **Pflicht-Readback:** was du zurücklesen MUSST, damit Strict-Mode zufrieden ist

Wind und QNH sind dynamisch — die Werte unten sind Beispiele.

---

### Phase 1 — Funkprobe (optional, aber gute Warm-up-Übung)

COM1 auf **127.575** (ATIS) oder **121.825** (Ground) tunen.

**Schritt 1 — Funkprobe**
- **Du:** „Friedrichshafen Rollkontrolle, Delta Echo X-Ray Yankee Zulu, Funkprobe 121,825."
- **Tower:** „Delta Echo X-Ray Yankee Zulu, Friedrichshafen Tower, Hoere Sie fuenf."
- **Pflicht-Readback:** keiner (RADIO_CHECK braucht kein Readback).
- **NfL-Anker:** §17 a) + §17 b/c).

> **Hinweis:** Aktuell antwortet das Tower-Template mit `Tower` statt `Rollkontrolle`,
> auch wenn du Ground tunst. Das ist eine Audit-Notiz für Bucket B Phase-2.

---

### Phase 2 — Rollanmeldung & Rollfreigabe

COM1 auf **121.825** (Ground) tunen.

**Schritt 2 — Erstanruf Ground + Rollanmeldung**

- **Du:** „Friedrichshafen Rollkontrolle, Delta Echo X-Ray Yankee Zulu, Cessna 172,
  GA-Vorfeld, Information Alfa, erbitte Rollen."
- **Tower:** „Delta Echo X-Ray Yankee Zulu, Friedrichshafen Rollkontrolle, guten Tag,
  rollen Sie zum Rollhalt Piste 24 ueber Alpha, QNH 1013."
- **Pflicht-Readback (NfL §25 b) Nr. 1):**
  - Callsign — `Delta Echo X-Ray Yankee Zulu` (oder verkürzt, **erst nachdem der Tower verkürzt**)
  - Piste — `Piste 24`
  - QNH — `QNH 1013`
- **Du:** „Rollen zum Rollhalt Piste 24 ueber Alpha, QNH 1013, Delta Echo X-Ray Yankee Zulu."
- **Tower (silent absorb):** kein Audio — Plugin geht in `TAXI_CLEARED`.
- **NfL-Anker:** §14, ANLAGE 1.4.7, §25 b) Nr. 1.

> **🧪 Negativ-Test 1 — QNH absichtlich weglassen**
>
> Lies stattdessen zurück: „Rollen Piste 24 ueber Alpha, Delta Echo X-Ray Yankee Zulu."
>
> Strict-Mode-Erwartung: Tower antwortet
> `"Delta Echo X-Ray Yankee Zulu, wiederholen Sie vollstaendig mit QNH."`
> und der State bleibt `IDLE` (kein Advance zu `TAXI_CLEARED`). Du musst nochmal
> sauber zurücklesen.
>
> **Im Log.txt erscheint:** `BZF strict: readback missing qnh`

> **🧪 Negativ-Test 2 — Piste UND QNH weglassen**
>
> Lies zurück: „Verstanden, rollen, Delta Echo X-Ray Yankee Zulu."
>
> Strict-Mode-Erwartung: Tower antwortet
> `"Delta Echo X-Ray Yankee Zulu, wiederholen Sie die vollstaendige Freigabe."`
> (Multi-Missing-Template). Log: `BZF strict: readback missing runway,qnh`.

---

### Phase 3 — Frequenzwechsel zum Tower

**Schritt 3 — Am Rollhalt: Frequenzwechsel anfordern (optional, manche Lotsen
geben den Wechsel mit der Rollfreigabe schon mit; das aktuelle Template
weist dich beim Erreichen des Rollhalts an).**

Wenn du am Rollhalt bist:

- **Du:** „Friedrichshafen Rollkontrolle, Delta Echo X-Ray Yankee Zulu, am Rollhalt
  Piste 24, abflugbereit."

Das Plugin erkennt das als `READY_FOR_DEPARTURE` und sendet dich auf die Tower-Frequenz:

- **Tower:** „Delta Echo X-Ray Yankee Zulu, verstanden, kontaktieren Sie Tower auf 120,075."
- **Pflicht-Readback (NfL §25 b) Nr. 1):**
  - Callsign — `Delta Echo X-Ray Yankee Zulu`
  - Frequenz — `120,075`
- **Du:** „120,075, Delta Echo X-Ray Yankee Zulu."

> **🧪 Negativ-Test 3 — Frequenz weglassen**
>
> Lies zurück: „Verstanden, Delta Echo X-Ray Yankee Zulu."
>
> Strict-Mode-Erwartung:
> `"Delta Echo X-Ray Yankee Zulu, wiederholen Sie vollstaendig mit Frequenz."`

COM1 auf **120.075** (Tower) tunen.

---

### Phase 4 — Erstanruf Tower & Startfreigabe

**Schritt 4 — Erstanruf Tower**

- **Du:** „Friedrichshafen Turm, Delta Echo X-Ray Yankee Zulu, Rollhalt Piste 24,
  abflugbereit."
- **Tower:** „Delta Echo X-Ray Yankee Zulu, Friedrichshafen Tower, Piste 24,
  Start frei, Wind 240 Grad 8 Knoten, melden Sie im Gegenanflug."
- **Pflicht-Readback:**
  - Callsign — `Delta Echo X-Ray Yankee Zulu`
  - Piste — `Piste 24`
- **Du:** „Piste 24, Start frei, Delta Echo X-Ray Yankee Zulu."
- **NfL-Anker:** ANLAGE 1.4.10/1.4.11, §25 b) Nr. 1 ii.

> **🧪 Negativ-Test 4 — Pistennummer im Startfreigabe-Readback weglassen**
>
> Lies zurück: „Start frei, Delta Echo X-Ray Yankee Zulu."
>
> Strict-Mode-Erwartung:
> `"Delta Echo X-Ray Yankee Zulu, wiederholen Sie vollstaendig mit Pistennummer."`

Jetzt **starten**.

---

### Phase 5 — Platzrundenmeldungen

Wenn du im Gegenanflug bist (Pattern Höhe ≈ 2500 ft MSL):

**Schritt 5 — Gegenanflug melden**
- **Du:** „Friedrichshafen Turm, Delta Echo X-Ray Yankee Zulu, Gegenanflug Piste 24."
- **Tower:** „Delta Echo X-Ray Yankee Zulu, Nummer eins, Piste 24, weiter Anflug,
  melden Sie Endanflug." (oder Variante)
- **Pflicht-Readback:** Callsign + Piste (keine neue QNH/Freq-Vergabe)

**Schritt 6 — Endanflug melden**
- **Du:** „Endanflug Piste 24, Delta Echo X-Ray Yankee Zulu."
- **Tower:** „Delta Echo X-Ray Yankee Zulu, Piste 24, Landung frei, Wind 240 Grad 8 Knoten."
- **Pflicht-Readback:**
  - Callsign — `Delta Echo X-Ray Yankee Zulu`
  - Piste — `Piste 24`
- **Du:** „Piste 24, Landung frei, Delta Echo X-Ray Yankee Zulu."
- **NfL-Anker:** ANLAGE 1.4.16 a).

> **🧪 Negativ-Test 5 — Pistennummer im Landefreigabe-Readback weglassen**
>
> Lies zurück: „Landung frei, Delta Echo X-Ray Yankee Zulu."
>
> Strict-Mode-Erwartung:
> `"Delta Echo X-Ray Yankee Zulu, wiederholen Sie vollstaendig mit Pistennummer."`
>
> Die Landefreigabe gilt **erst** wenn du sauber zurückgelesen hast (Plugin
> hält den State bei `LANDING_CLEARED`-Vorbereitung).

---

### Phase 6 — Pistenverlassen & zurück zum Vorfeld

**Schritt 7 — Piste verlassen**
- **Du (nach dem Aufsetzen, beim Verlassen der Piste):** „Delta Echo X-Ray Yankee Zulu, Piste verlassen."
- **Tower:** „Delta Echo X-Ray Yankee Zulu, verstanden, kontaktieren Sie Rollkontrolle
  auf 121,825, auf Wiederhoeren."
- **Pflicht-Readback:**
  - Callsign — `Delta Echo X-Ray Yankee Zulu`
  - Frequenz — `121,825`
- **Du:** „121,825, Delta Echo X-Ray Yankee Zulu."
- **NfL-Anker:** ANLAGE 1.4.7 *z), ANLAGE 1.4.20.

> **🧪 Negativ-Test 6 — Frequenz im Pistenfrei-Readback weglassen**
>
> Lies zurück: „Verstanden, Delta Echo X-Ray Yankee Zulu."
>
> Strict-Mode-Erwartung:
> `"Delta Echo X-Ray Yankee Zulu, wiederholen Sie vollstaendig mit Frequenz."`

COM1 auf **121.825** (Ground) tunen.

**Schritt 8 — Rollanforderung zur Abstellposition**
- **Du:** „Friedrichshafen Rollkontrolle, Delta Echo X-Ray Yankee Zulu, Piste verlassen,
  erbitte Rollen zum GA-Vorfeld."
- **Tower:** „Delta Echo X-Ray Yankee Zulu, rollen Sie zum GA-Vorfeld ueber Alpha."
- **Pflicht-Readback:** Callsign — keine QNH/Freq-Neuvergabe in diesem Template.
- **Du:** „Rollen GA-Vorfeld ueber Alpha, Delta Echo X-Ray Yankee Zulu."

**Schritt 9 — Frequenz verlassen**
- **Du (am Abstellplatz, Engines aus):** „Friedrichshafen Rollkontrolle,
  Delta Echo X-Ray Yankee Zulu, verlasse Frequenz."
- **Tower:** „Delta Echo X-Ray Yankee Zulu, Verlassen der Frequenz genehmigt,
  auf Wiederhoeren."
- **Pflicht-Readback:** keiner (`LEAVING_FREQUENCY` ist self-terminating).

---

## Touch-and-Go-Variante (statt Schritt 6)

Wenn du nicht voll landen willst, sondern Touch-and-Go für mehr Übung:

- **Du (anstelle der Landefreigabe-Anfrage):** „Endanflug Piste 24, aufsetzen und durchstarten,
  Delta Echo X-Ray Yankee Zulu."
- **Tower:** „Delta Echo X-Ray Yankee Zulu, Piste 24, frei zum Aufsetzen und Durchstarten,
  Wind 240 Grad 8 Knoten."
- **Pflicht-Readback:** Callsign + Piste.
- **Du:** „Piste 24, frei zum Aufsetzen und Durchstarten, Delta Echo X-Ray Yankee Zulu."
- **NfL-Anker:** ANLAGE 1.4.16 c).

Pattern fortsetzen — Gegenanflug → Queranflug → Endanflug.

---

## Optionaler VRP-Abstecher (wenn du warm bist)

Statt direkt landen: in den Gegenanflug zum VRP Whiskey ausfliegen, ein paar Minuten
herumfliegen, dann via Sierra wieder einfliegen.

**Departure-Variante in Schritt 4:**
- **Du:** „Friedrichshafen Turm, Delta Echo X-Ray Yankee Zulu, Rollhalt Piste 24,
  abflugbereit, VFR ueberlandflug Richtung Whiskey."
- **Tower:** „Delta Echo X-Ray Yankee Zulu, Friedrichshafen Tower, Piste 24, Start frei,
  Wind 240 Grad 8 Knoten, Kurs nach Plan freigegeben, Frequenzwechsel nach Start genehmigt."

→ Plugin geht in `XC/DEPARTURE_CLEARED`. Du kannst frei fliegen.

**Rückflug-Variante über VRP Sierra:**

Position bei Sierra (ca. 47.61° N, 9.59° E, 3000 ft):
- **Du:** „Friedrichshafen Turm, Delta Echo X-Ray Yankee Zulu, Cessna 172, ueber Sierra,
  3000 Fuss, Information Alfa, zur Landung."
- **Tower:** „Delta Echo X-Ray Yankee Zulu, Friedrichshafen Tower, frei zum Einflug in die
  Kontrollzone ueber Sierra, Piste 24, melden Sie im Gegenanflug."
- **Pflicht-Readback:** Callsign + Piste.

Dann Phase 5 + 6 wie oben.

---

## Was du im X-Plane Log.txt beobachten kannst

Strict-Mode loggt jede Verletzung:

```
[xp_wellys_atc] BZF strict: readback missing qnh
[xp_wellys_atc] BZF strict: readback missing runway,qnh
[xp_wellys_atc] BZF strict: readback missing frequency
```

Diese Zeilen siehst du nur wenn der Strict-Mode aktiv ist (DE-Profil + Toggle an)
und ein READBACK-Intent ein Pflichtelement vermisst.

Außerdem oben in Log.txt der Backend-Anker:
```
BACKEND MODE: local
```
oder `BACKEND MODE: openai` — Audit-Pfad für die Sprache-erste / Cloud-Variante.

---

## Bekannte Beschränkungen — MVP

Folgendes ist im Phase-2-MVP bewusst **noch nicht** implementiert und kann den Test
**nicht** abdecken:

| Was | Warum nicht im MVP | Wo dokumentiert |
|---|---|---|
| **Erstanruf-Pflichtelement-Check** — z. B. „du hast Typ + Position + ATIS im Erstanruf weggelassen" | Bucket A Phase-1 prüft nur READBACK gegen die letzte Tower-Clearance, nicht den ersten Pilot-Call. Die Daten dafür (`required_elements` pro Intent in `intent_rules.json`) sind nicht hinterlegt. | [`bzf_coverage.md`](bzf_coverage.md) §1.2/1.5/1.6 → Bucket A Phase-2 |
| **MAYDAY / PAN-PAN** | Cross-Profile-Engine-Feature, kein DE-Gap. Eigene Milestone. | [`bzf_coverage.md`](bzf_coverage.md) §9 → „MAYDAY-Move" |
| **Squawk-Code-Anweisung & Readback** | Intent + Template fehlen (`REQUEST_SQUAWK` / `SQUAWK_ASSIGNMENT`). Kommt als eigener Mini-PR. | [`bzf_coverage.md`](bzf_coverage.md) §12.7–12.10 |
| **RMZ-Einflug- / Verlassen-Meldungen** | Neue Intents + Templates nötig. Eigener Mini-PR. | [`bzf_coverage.md`](bzf_coverage.md) §16.1/16.2 |
| **„Sind Sie in Verfügung der freigegebenen Strecke?"** | Existiert in NfL 2024 nicht — wahrscheinlich Paraphrase aus IFR-Kontext. | [`bzf_coverage.md`](bzf_coverage.md) §15.2 |
| **Konditionelle Freigaben** („BEHIND landendem Verkehr") | Pflichtelement-Wiederholung der Bedingung ist nicht modelliert. | [`bzf_coverage.md`](bzf_coverage.md) §3.4/4.3 |
| **„Lesbarkeit X"-Variante** (variabel statt konstant 5) | Verständlichkeitsskala 1–5 als Daten-Tabelle ist nicht hinterlegt — aktuell konstant „Hoere Sie fuenf". | [`bzf_coverage.md`](bzf_coverage.md) §2.3 |
| **Strict-Mode-Filter auf Sim-Mode-Keywords** („startbereit", „piste frei") | Bei Strict-Mode aktiv akzeptiert das Plugin diese umgangssprachlichen Varianten trotzdem — der Tower antwortet zwar in NfL-Form, aber das Pilot-Eingangs-Vokabular bleibt tolerant. | [`bzf_coverage.md`](bzf_coverage.md) §4.1/7.1 |

---

## Wenn etwas unerwartet ist

1. **Strict-Mode-Toggle ist nicht sichtbar** → Profile auf `DE/BZF` setzen (Toggle ist DE-only).
2. **Strict-Mode greift nicht** → `Log.txt` checken auf:
   - `[xp_wellys_atc] BACKEND MODE: ...` — Plugin überhaupt aktiv?
   - `BZF strict: readback missing ...` — Strict-Check läuft?
   - Wenn das Plugin den READBACK gar nicht erkennt: prüfe ob `Verstanden` / `Roger` / Callsign im Pilot-Transkript sind (`intent_rules.json:98`).
3. **Tower wiederholt sich endlos** → State-Machine ist auf Konformitäts-Wartezeit. Setze Disregard (`atc_session::disregard()` über UI-Reset-Button) und versuche nochmal.
4. **Korrektive Tower-Antwort ist generisch** („wiederholen Sie die vollstaendige Freigabe") statt spezifisch → 2 oder mehr Elemente fehlen, das ist `missing_multi`-Template (`atc_templates.json :: bzf_strict.missing_multi`). Funktion korrekt.

---

## Feedback an die Coverage-Matrix

Falls du beim Testen Phrasen findest, die nicht NfL-konform klingen oder fehlen:

1. Zeile in [`bzf_coverage.md`](bzf_coverage.md) referenzieren (z. B. „Row 6.3").
2. NfL-§-Verweis + Vorschlag.
3. GitHub-Issue oder direkt PR — siehe README §Known Limitations.
