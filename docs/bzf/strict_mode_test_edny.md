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

## Wichtig: EDNY ist tower_only

Friedrichshafen hat **keine separate Bodenkontrolle**. Der Tower auf
**120.075** macht Boden + Air. Das Plugin erkennt das aus apt.dat
(`ctx.tower_only=true`) und routet:

- Rollanmeldung direkt an den Tower (kein „Rollkontrolle"-Anruf)
- Variable `{taxi_controller}` wird auf `"Tower"` gesetzt
- Nach Landung kein „kontaktieren Sie Rollkontrolle" — Tower gibt die
  Roll-Anweisung zum GA-Vorfeld direkt
- COM1 bleibt die ganze Zeit auf der Tower-Frequenz

Das heißt für den Test: **eine Frequenz, eine Funkstelle** — alle
Strict-Mode-Checks laufen über genau diesen Tower-Channel.

---

## Vorbereitung

### Plugin bauen & installieren

Das Strict-Mode-Feature ist im aktuellen Working-Tree, noch nicht im
installierten Plugin:

```bash
make build && make install
```

### Plugin-Settings

1. **Profile** auf `DE/BZF` setzen
   (Settings-Tab → ATC-Profil → `DE/BZF`)
2. **BZF-Strict-Mode** einschalten
   (Settings-Tab → Checkbox „BZF-Strict-Mode (Tower prueft Pflicht-Readback)")
   → Toggle ist **nur sichtbar** wenn Profile=DE.
3. **Pilot-Callsign** auf `HB-DSV` setzen
   (Settings-Tab → Pilot-Callsign → `HB-DSV`)
   → Das Plugin expandiert das beim Sprechen zu
     `Hotel Bravo Delta Sierra Victor` via `de_phraseology::expand_callsign_phonetic()`.
4. **Backend-Modus:** beliebig (Lokal oder OpenAI — Strict-Mode ist backend-agnostisch).

### X-Plane Setup

- **Aircraft:** DV20 (Diamond DA20 Katana) oder vergleichbare GA-Maschine
- **Flugplatz:** `EDNY` (Friedrichshafen)
- **Position:** GA-Vorfeld / Apron (Cold Start)
- **Engines:** aus
- **Wetter:** beliebig — der Tower nutzt das aktuelle X-Plane-Wetter für
  Wind und QNH

### EDNY-Eckdaten

| | |
|---|---|
| ATIS | 127.575 |
| Tower / TWR-RDR | **120.075** (kombiniert Tower + Boden + Director) |
| Pisten | 06 / 24 (beide left-traffic) |
| Pattern-Höhe | ca. 2500 ft MSL (≈ 1100 ft AGL) |
| VRPs | November (N), Oscar (NE), Sierra (S), Whiskey (W) |

---

## Flugverlauf — Platzrunde

Pro Schritt steht:
- **Du:** was du auf PTT sagen sollst
- **Tower:** was du als Antwort erwartest
- **Pflicht-Readback:** was du zurücklesen MUSST, damit Strict-Mode zufrieden ist

Wind, QNH und ATIS-Letter sind dynamisch — die Werte unten sind Beispiele.
Du musst die Werte aus der jeweiligen ATIS-Aussendung oder Tower-Clearance übernehmen.

COM1 die ganze Zeit auf **120.075** (Tower).

---

### Phase 1 — Funkprobe (optional, gute Warm-up-Übung)

**Schritt 1**
- **Du:** „Friedrichshafen Tower, Hotel Bravo Delta Sierra Victor, Funkprobe 120,075."
- **Tower:** „Hotel Bravo Delta Sierra Victor, Friedrichshafen Tower, Hoere Sie fuenf."
- **Pflicht-Readback:** keiner (RADIO_CHECK braucht kein Readback).
- **NfL-Anker:** §17 a) + §17 b/c).

---

### Phase 2 — Rollanmeldung & Rollfreigabe (direkt am Tower)

**Schritt 2 — Erstanruf Tower + Rollanmeldung**

- **Du:** „Friedrichshafen Tower, Hotel Bravo Delta Sierra Victor, DV20,
  GA-Vorfeld, Information Alfa, erbitte Rollen."
- **Tower:** „Hotel Bravo Delta Sierra Victor, Friedrichshafen Tower, guten Tag,
  rollen Sie zum Rollhalt Piste 24 ueber Alpha, QNH 1013."
- **Pflicht-Readback (NfL §25 b) Nr. 1):**
  - Callsign — `Hotel Bravo Delta Sierra Victor` (oder verkürzt, **erst nachdem der Tower verkürzt**)
  - Piste — `Piste 24`
  - QNH — `QNH 1013`
- **Du:** „Rollen zum Rollhalt Piste 24 ueber Alpha, QNH 1013, Hotel Bravo Delta Sierra Victor."
- **Tower (silent absorb):** kein Audio — Plugin geht in `TAXI_CLEARED`.
- **NfL-Anker:** §14, ANLAGE 1.4.7, §25 b) Nr. 1.

> **🧪 Negativ-Test 1 — QNH absichtlich weglassen**
>
> Lies stattdessen zurück: „Rollen Piste 24 ueber Alpha, Hotel Bravo Delta Sierra Victor."
>
> Strict-Mode-Erwartung: Tower antwortet
> `"Hotel Bravo Delta Sierra Victor, wiederholen Sie vollstaendig mit QNH."`
> und der State bleibt `IDLE` (kein Advance zu `TAXI_CLEARED`). Du musst nochmal
> sauber zurücklesen.
>
> **Im Log.txt erscheint:** `BZF strict: readback missing qnh`

> **🧪 Negativ-Test 2 — Piste UND QNH weglassen**
>
> Lies zurück: „Verstanden, rollen, Hotel Bravo Delta Sierra Victor."
>
> Strict-Mode-Erwartung: Tower antwortet
> `"Hotel Bravo Delta Sierra Victor, wiederholen Sie die vollstaendige Freigabe."`
> (`missing_multi`-Template). Log: `BZF strict: readback missing runway,qnh`.

> **🧪 Negativ-Test 3 — Callsign weglassen**
>
> Lies zurück: „Rollen Piste 24 ueber Alpha, QNH 1013."
>
> Strict-Mode-Erwartung: Tower antwortet
> `"Hotel Bravo Delta Sierra Victor, lesen Sie immer mit vollem Rufzeichen zurueck."`
> Log: `BZF strict: readback missing callsign`.

---

### Phase 3 — Startbereit & Startfreigabe (gleiche Frequenz)

Am Rollhalt angekommen — kein Frequenzwechsel nötig, du bist schon am Tower.

**Schritt 3 — Startbereit-Meldung**

- **Du:** „Friedrichshafen Tower, Hotel Bravo Delta Sierra Victor, Rollhalt Piste 24,
  abflugbereit."
- **Tower:** „Hotel Bravo Delta Sierra Victor, Friedrichshafen Tower, Piste 24,
  Start frei, Wind 240 Grad 8 Knoten, melden Sie im Gegenanflug."
- **Pflicht-Readback:**
  - Callsign — `Hotel Bravo Delta Sierra Victor`
  - Piste — `Piste 24`
- **Du:** „Piste 24, Start frei, Hotel Bravo Delta Sierra Victor."
- **NfL-Anker:** ANLAGE 1.4.10/1.4.11, §25 b) Nr. 1 ii.

> **🧪 Negativ-Test 4 — Pistennummer im Startfreigabe-Readback weglassen**
>
> Lies zurück: „Start frei, Hotel Bravo Delta Sierra Victor."
>
> Strict-Mode-Erwartung:
> `"Hotel Bravo Delta Sierra Victor, wiederholen Sie vollstaendig mit Pistennummer."`

Jetzt **starten**.

---

### Phase 4 — Platzrundenmeldungen

Wenn du im Gegenanflug bist (Pattern-Höhe ≈ 2500 ft MSL):

**Schritt 4 — Gegenanflug melden**
- **Du:** „Friedrichshafen Tower, Hotel Bravo Delta Sierra Victor, Gegenanflug Piste 24."
- **Tower:** „Hotel Bravo Delta Sierra Victor, Nummer eins, Piste 24, weiter Anflug,
  melden Sie Endanflug." (oder Variante)
- **Pflicht-Readback:** Callsign + Piste (keine neue QNH/Freq-Vergabe)

**Schritt 5 — Endanflug melden**
- **Du:** „Endanflug Piste 24, Hotel Bravo Delta Sierra Victor."
- **Tower:** „Hotel Bravo Delta Sierra Victor, Piste 24, Landung frei, Wind 240 Grad 8 Knoten."
- **Pflicht-Readback:**
  - Callsign — `Hotel Bravo Delta Sierra Victor`
  - Piste — `Piste 24`
- **Du:** „Piste 24, Landung frei, Hotel Bravo Delta Sierra Victor."
- **NfL-Anker:** ANLAGE 1.4.16 a).

> **🧪 Negativ-Test 5 — Pistennummer im Landefreigabe-Readback weglassen**
>
> Lies zurück: „Landung frei, Hotel Bravo Delta Sierra Victor."
>
> Strict-Mode-Erwartung:
> `"Hotel Bravo Delta Sierra Victor, wiederholen Sie vollstaendig mit Pistennummer."`
>
> Die Landefreigabe gilt **erst** wenn du sauber zurückgelesen hast (Plugin
> hält den State bei `LANDING_CLEARED`-Vorbereitung).

---

### Phase 5 — Pistenverlassen & zurück zum Vorfeld (Tower-Only-Flow)

**Schritt 6 — Piste verlassen**
- **Du (nach dem Aufsetzen, beim Verlassen der Piste):** „Hotel Bravo Delta Sierra Victor, Piste verlassen."
- **Tower (Tower-Only-Template):** „Hotel Bravo Delta Sierra Victor, verstanden,
  rollen Sie zum GA-Vorfeld ueber Alpha, auf Wiederhoeren."
- **Pflicht-Readback:** Callsign — keine Frequenz, keine Piste, kein QNH in diesem Template.
- **Du:** „Rollen GA-Vorfeld ueber Alpha, Hotel Bravo Delta Sierra Victor."
- **NfL-Anker:** ANLAGE 1.4.7 *z), ANLAGE 1.4.20.

> Beachte: Weil EDNY `tower_only=true` ist, kommt **kein** „kontaktieren Sie
> Rollkontrolle auf 121,825" wie bei einem 2-stufigen Platz (z. B. EDDS / EDDF).
> Das `RUNWAY_VACATED_TOWER_ONLY`-Template in `atc_templates.json:71/300/327`
> wird automatisch gewählt sobald `ctx.tower_only=true` ist.

**Schritt 7 — Frequenz verlassen (am Abstellplatz)**
- **Du:** „Friedrichshafen Tower, Hotel Bravo Delta Sierra Victor, verlasse Frequenz."
- **Tower:** „Hotel Bravo Delta Sierra Victor, Verlassen der Frequenz genehmigt,
  auf Wiederhoeren."
- **Pflicht-Readback:** keiner (`LEAVING_FREQUENCY` ist self-terminating).

---

## Touch-and-Go-Variante (statt Schritt 5)

Wenn du nicht voll landen willst, sondern Touch-and-Go für mehr Übung:

- **Du (anstelle der Landefreigabe-Anfrage):** „Endanflug Piste 24, aufsetzen und durchstarten,
  Hotel Bravo Delta Sierra Victor."
- **Tower:** „Hotel Bravo Delta Sierra Victor, Piste 24, frei zum Aufsetzen und Durchstarten,
  Wind 240 Grad 8 Knoten."
- **Pflicht-Readback:** Callsign + Piste.
- **Du:** „Piste 24, frei zum Aufsetzen und Durchstarten, Hotel Bravo Delta Sierra Victor."
- **NfL-Anker:** ANLAGE 1.4.16 c).

Pattern fortsetzen — Gegenanflug → Queranflug → Endanflug.

---

## Optionaler VRP-Abstecher (wenn du warm bist)

Statt direkt landen: in den Gegenanflug zum VRP Whiskey ausfliegen, ein paar Minuten
herumfliegen, dann via Sierra wieder einfliegen.

**Departure-Variante in Schritt 3 (Startbereit):**
- **Du:** „Friedrichshafen Tower, Hotel Bravo Delta Sierra Victor, Rollhalt Piste 24,
  abflugbereit, VFR ueberlandflug Richtung Whiskey."
- **Tower:** „Hotel Bravo Delta Sierra Victor, Friedrichshafen Tower, Piste 24, Start frei,
  Wind 240 Grad 8 Knoten, Kurs nach Plan freigegeben, Frequenzwechsel nach Start genehmigt."

→ Plugin geht in `XC/DEPARTURE_CLEARED`. Du kannst frei fliegen und brauchst
   nicht beim Tower bleiben (Frequenzwechsel nach Start ist genehmigt).

**Rückflug-Variante über VRP Sierra:**

Position bei Sierra (ca. 47.61° N, 9.59° E, 3000 ft):
- COM1 zurück auf **120.075**.
- **Du:** „Friedrichshafen Tower, Hotel Bravo Delta Sierra Victor, DV20, ueber Sierra,
  3000 Fuss, Information Alfa, zur Landung."
- **Tower:** „Hotel Bravo Delta Sierra Victor, Friedrichshafen Tower, frei zum Einflug in die
  Kontrollzone ueber Sierra, Piste 24, melden Sie im Gegenanflug."
- **Pflicht-Readback:** Callsign + Piste.

Dann Phase 4 + 5 wie oben.

---

## Was du im X-Plane Log.txt beobachten kannst

Strict-Mode loggt jede Verletzung:

```
[xp_wellys_atc] BZF strict: readback missing qnh
[xp_wellys_atc] BZF strict: readback missing runway,qnh
[xp_wellys_atc] BZF strict: readback missing callsign
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
| **`taxi_controller`="Tower" statt "Turm" bei tower_only DE** | `ground_operations.cpp:181` setzt bei tower_only immer "Tower", auch im DE-Profil. NfL §34 erlaubt beide („TOWER" und „TURM"), aber konsistent wäre für DE → „Turm". | nicht in coverage.md — eigener Mini-Fix |

---

## Rettungsanker — „Wiederholen Sie" (REQUEST_REPEAT)

Wenn du beim Readback merkst, du hast die QNH-Zahl oder Pistennummer vergessen
und der Strict-Mode dich gerade auf einen Fehler hingewiesen hat:

- **Du:** „Friedrichshafen Turm, Hotel Bravo Delta Sierra Victor, wiederholen Sie."
- **Tower:** spielt die letzte echte Clearance noch einmal ab (NICHT die
  Strict-Mode-Korrektur — die wird übersprungen).

Das ist NfL §18 c) Nr. 4 Standardphrase. Erkannt werden auch:
„sagen Sie nochmals", „sagen Sie noch einmal", „say again", „noch einmal bitte".

State und Readback-Erwartung bleiben dabei unverändert — du musst danach
immer noch korrekt zurücklesen.

Außerdem: Der **Transcript-Tab** im ATC-Panel zeigt dir den ganzen Funkverlauf,
falls du zwischendurch nachgucken willst (z. B. „Piste 24 oder 06?").

---

## Wenn etwas unerwartet ist

1. **Strict-Mode-Toggle ist nicht sichtbar** → Profile auf `DE/BZF` setzen (Toggle ist DE-only).
2. **Strict-Mode greift nicht** → `Log.txt` checken auf:
   - `[xp_wellys_atc] BACKEND MODE: ...` — Plugin überhaupt aktiv?
   - `BZF strict: readback missing ...` — Strict-Check läuft?
   - Wenn das Plugin den READBACK gar nicht erkennt: prüfe ob `Verstanden` / `Roger` / Callsign im Pilot-Transkript sind (`intent_rules.json:98`).
3. **Tower wiederholt sich endlos** → State-Machine wartet auf sauberen Readback. Setze Disregard (UI-Reset-Button) und versuche nochmal.
4. **Korrektive Tower-Antwort ist generisch** („wiederholen Sie die vollstaendige Freigabe") statt spezifisch → 2 oder mehr Elemente fehlen, das ist das `missing_multi`-Template. Funktion korrekt.
5. **Plugin gibt mir trotz Tower-Only eine Ground-Frequenz raus** → `ctx.tower_only` wird aus apt.dat abgeleitet. Wenn EDNY in deinem X-Plane mit einem benutzerdefinierten Szenenpaket eine künstliche Ground-Frequenz hat, kann das Plugin sie sehen. Prüfe Log.txt auf den airport_freqs-Dump beim Plugin-Start.

---

## Feedback an die Coverage-Matrix

Falls du beim Testen Phrasen findest, die nicht NfL-konform klingen oder fehlen:

1. Zeile in [`bzf_coverage.md`](bzf_coverage.md) referenzieren (z. B. „Row 6.3").
2. NfL-§-Verweis + Vorschlag.
3. GitHub-Issue oder direkt PR — siehe README §Known Limitations.
