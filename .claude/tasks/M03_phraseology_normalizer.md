# M3 — BZF-Phraseology-Normalizer (Aussprache-Korrektheit)

**Plan-Referenz:** `~/.claude/plans/kannst-du-mir-einen-jolly-flamingo.md`
**Voraussetzung:** M2 (Templates existieren als Inputs)
**Folge-Milestones:** M5 (OpenAI nutzt Normalizer), M6 (Local nutzt
Normalizer), M7 (Reverse-Normalizer im selben Modul)

## Ziel
Realistische deutsche Funkaussprache nach BZF-Standard. Zahlen werden
ziffernweise gesprochen, "zwei" wird zu "zwo", Frequenzen / Höhen /
Steuerkurse / Pisten folgen dem AIP-Phraseologie-Standard. Dies ist
der zentrale Differenzierungs-Wert der DE-Region — generische
TTS-Aussprache ("Piste fuenfundzwanzig") wäre wertlos.

## Aussprache-Regeln (BZF-Referenz)
- **Ziffern:** `null, eins, zwo, drei, vier, fuenf, sechs, sieben, acht,
  neun` — "zwo" mandatory statt "zwei" (Verwechslungsgefahr mit "drei")
- **Frequenzen:** ziffernweise mit "Komma" — `118.300` →
  `"eins eins acht Komma drei null null"`
- **Steuerkurse:** dreistellig — Heading 050 →
  `"Steuerkurs null fuenf null"`
- **Pisten:** ziffernweise — `"Piste 25"` → `"Piste zwo fuenf"`;
  `"Piste 07L"` → `"Piste null sieben links"`
- **QNH:** `"QNH eins null eins drei Hektopascal"` (1013)
- **NATO-Alphabet** in deutscher Aussprache: Alfa, Bravo, Charlie, ...,
  Zulu (NICHT das alte deutsche Alphabet Anton/Berta — BZF-Standard
  ist NATO)
- **Callsigns:** Buchstaben einzeln, Zahlen wie oben — `"D-EXYZ"` →
  `"Delta Echo X-Ray Yankee Zulu"`
- **Höhen:** "drei tausend fuenfhundert Fuss" (3500 ft) — Höhen über
  1000 ft dürfen "tausend"/"hundert" verwenden, nicht ziffernweise

## Architektur

- **Neues Modul:** `src/atc/de_phraseology.{hpp,cpp}` in der Engine
  OBJECT lib (SDK-frei, damit `atc_repl` und Tests es nutzen können).
  - `std::string normalize_for_speech(const std::string& text)` —
    Pre-TTS-Normalizer
  - Idempotent: doppelte Normalisierung erzeugt keine Schäden
  - Heuristik: erkennt Zahlen-Patterns im aviation-Kontext (vor
    "Piste", "QNH", "Komma", "Steuerkurs") und expandiert ziffernweise.
    Numerische Höhen mit ≥ 1000 → "tausend"/"hundert".

- **Integration:**
  - `atc_session.cpp` ruft den Normalizer vor `tts().speak(...)` auf,
    wenn `flow_region() == "DE"`. Andere Regionen unverändert.
  - **Symmetrisch (siehe M7):** Der Intent-Parser muss "zwo" → 2,
    "null sieben" → 07, "eins null eins drei" → 1013 verstehen.
    Reverse-Normalisierung gehört zur Intent-Parser-Milestone und lebt
    im selben Modul.

- **Templates-Regel:** Templates bleiben mit **Ziffern-Platzhaltern**
  (`"Piste {runway}"`, `"QNH {qnh}"`). Templates dürfen NICHT bereits
  "zwo" oder "null sieben" enthalten — sonst wird bei Schemaänderung
  doppelt normalisiert. Das ist eine Test-Invariante.

## Kritische Dateien
- **Neu:** `src/atc/de_phraseology.{hpp,cpp}`
- `src/atc/atc_session.cpp` — vor jedem `tts().speak(...)` Normalizer
  einschieben, wenn Region == DE
- **Neu:** `tests/test_de_phraseology.cpp` — ~30 Testfälle
  (Pisten, QNH, Frequenzen, Steuerkurse, Callsigns, Höhen,
  Edge-Cases wie "Piste 36L", "Sicht 5000")
- `CMakeLists.txt` — neue TU in `xp_atc_engine` OBJECT lib

## Testdaten (Beispiele)

| Input | Expected Output |
|---|---|
| `"Piste 25 links"` | `"Piste zwo fuenf links"` |
| `"QNH 1013"` | `"QNH eins null eins drei Hektopascal"` |
| `"Frequenz 118.300"` | `"Frequenz eins eins acht Komma drei null null"` |
| `"Steuerkurs 050"` | `"Steuerkurs null fuenf null"` |
| `"D-EXYZ"` | `"Delta Echo X-Ray Yankee Zulu"` |
| `"3500 Fuss"` | `"drei tausend fuenfhundert Fuss"` |
| `"Information Alpha"` | `"Information Alfa"` |

## Risiko
Über- oder Unter-Normalisierung. Wenn ein Wort wie "300" sowohl als
"drei hundert" (Höhe) als auch "drei null null" (Frequenz) auftauchen
kann, muss der Normalizer Kontext erkennen. Mitigation: Kontext-Tokens
vor der Zahl prüfen ("Piste"/"Steuerkurs"/"Komma" → ziffernweise;
"Fuss"/"Meter" → "tausend"/"hundert"). Default bei Ambiguität:
ziffernweise (safer für Funkkontext).

## DoD
Tests grün, A/B-Listening-Test mit beiden Backends: 10 typische
BZF-Funksprüche abspielen, Aussprache mit echtem BZF-Audio (z. B.
DFS-Schulungsmaterial, LiveATC.net-Aufnahmen deutscher Plätze)
subjektiv vergleichen.
