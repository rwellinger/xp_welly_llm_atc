# M4 — UI-Lokalisierung DE

**Plan-Referenz:** `~/.claude/plans/kannst-du-mir-einen-jolly-flamingo.md`
**Voraussetzung:** M1 (DE-Region wählbar)
**Folge-Milestones:** keiner (Stand-Alone)

## Ziel
Wenn DE-Region aktiv ist, ist auch das in-sim ImGui-UI auf Deutsch.
Konsistenz: deutscher Funk + deutsches UI gehören zusammen, sonst wirkt
die Erfahrung gebrochen.

## Wichtige Randbedingung (`feedback_xplane_log_ascii.md`)
Die in-sim ImGui-Font rendert UTF-8-Sonderzeichen als `?`. UI-Strings
müssen daher **ASCII-safe deutsch** sein: `ae` / `oe` / `ue` / `ss`
statt `ä` / `ö` / `ü` / `ß` (z. B. "Einstellungen" bleibt OK,
"Schließen" → "Schliessen", "Über" → "Ueber"). Templates in
`atc_templates.json` sind davon **nicht** betroffen — die gehen nach
TTS, nicht ins UI.

## Mechanismus
- Neue Datei pro Region: `data/regions/{eu,us,de}/ui_strings.json` —
  flacher Key→String-Map. EU/US enthalten englische Strings (heutiger
  Stand), DE deutsche ASCII-safe-Strings.
- Neuer Helper: `src/ui/ui_strings.{hpp,cpp}` mit `init()` (lädt JSON
  beim Region-Wechsel), `tr(key)` → `const char*`, `reload()` für
  Hot-Reload.
- Alle hartcodierten UI-Strings in `src/ui/atc_ui.cpp` durch
  `tr("key")` ersetzen. Grobschätzung: ~80–120 Strings (Tab-Namen,
  Button-Labels, Status-Felder, Tooltips, Error-Messages).

## Scope der Lokalisierung
- Tab-Namen: "ATC" / "Settings" / "Models" / "Traffic" →
  "ATC" / "Einstellungen" / "Modelle" / "Verkehr"
- Status-Panel: "Frequencies" → "Frequenzen", "Active COM" →
  "Aktive COM", "Volume" → "Lautstaerke"
- Settings: "Backend Mode" → "Backend-Modus", "Region" → "Region",
  "Paste" → "Einfuegen", "Save Key" → "Schluessel speichern",
  "Delete Key" → "Schluessel loeschen"
- Models-Tab: "Download" → "Herunterladen", "Verify" → "Pruefen",
  "Progress" → "Fortschritt"
- Phraseology-Hints-Panel: nutzt bereits `phraseology_hints.json` aus
  M2, also schon DE-fähig.

## Kritische Dateien
- **Neu:** `src/ui/ui_strings.{hpp,cpp}`
- **Neu:** `data/regions/{eu,us,de}/ui_strings.json`
- `src/ui/atc_ui.cpp` — alle String-Literals durch `tr(...)` ersetzen.
- `src/main.cpp` — `ui_strings::init()` beim Plugin-Start, `reload()`
  beim Region-Wechsel (analog `atc_templates::reload()`).
- `src/persistence/settings.cpp::set_flow_region()` — am Ende
  `ui_strings::reload()` triggern, damit der Switch sofort greift.

## Optionaler Folge-Scope (nicht für MVP)
Unicode-fähige Font im ImGui-Stack (TTF mit erweitertem Glyph-Range) —
würde echte Umlaute ermöglichen. Aufwand: Font-Bundle (~500 KB),
ImGui-Init-Anpassung, Lizenz-Check für die Font. Als separate
Erweiterung tracken, nicht Voraussetzung für MVP.

## DoD
Region-Switch zu DE lokalisiert sofort alle UI-Strings (ohne
Plugin-Reload). Reverse-Test: Switch zurück zu EU/US → englisches UI
ist wieder da. Keine `?`-Glyph-Artefakte im UI.
