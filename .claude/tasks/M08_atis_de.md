# M8 — ATIS auf Deutsch

**Plan-Referenz:** `~/.claude/plans/kannst-du-mir-einen-jolly-flamingo.md`
**Voraussetzung:** M3 (Normalizer, damit ATIS-Zahlen BZF-korrekt
gesprochen werden), M5 oder M6 (Backend kann Deutsch)
**Folge-Milestones:** M9 (VRPs erweitern ATIS-Inhalt)

## Ziel
ATIS-Broadcast spielt automatisch auf Deutsch ab, wenn DE-Region aktiv
ist und der COM auf die ATIS-Frequenz steht.

## Kritische Dateien
- `src/atc/atis_generator.cpp:66, 232` — heute hartes
  `flow_region() == "US"` Branching. Erweitern um DE-Pfad:
  - Wind: "Wind 240 Grad 8 Knoten, boeig bis 12"
  - QNH: "QNH 1013 Hektopascal"
  - Visibility: "Sicht ueber 10 Kilometer" / "Sicht 8000 Meter"
  - Wolken: "wenige Wolken in 3500 Fuss" → ggf. "wenige Wolken in
    1100 Meter" — AIP folgen (Wolken auf VFR-Karten meist in Fuss).
  - Phonetisches Alphabet bleibt NATO (Alfa, Bravo, ... Zulu).
- Information-Letter-Wording: "Information Alfa aktuell, Piste 25 in
  Betrieb, ..."

## Normalizer-Interaktion
Die ATIS-Strings durchlaufen den M3-Normalizer vor TTS. Templates also
mit Ziffern schreiben ("Piste 25", "QNH 1013"), nicht ausgeschrieben.

## DoD
Auf EDNY-ATIS-Frequenz (123.075) spielt der Plugin eine deutsche ATIS
ab, mit korrekten Werten aus dem XPlaneContext.
