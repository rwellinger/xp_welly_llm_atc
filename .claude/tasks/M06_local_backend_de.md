# M6 — Local-Backend Deutsch-fähig (Apple Silicon)

**Plan-Referenz:** `~/.claude/plans/kannst-du-mir-einen-jolly-flamingo.md`
**Voraussetzung:** M3 (Normalizer), M5 (deutscher System-Prompt
existiert in `atc_prompt_templates.json`)
**Folge-Milestones:** Backend-Vergleich in Verification

## Ziel
Local-Stack (whisper.cpp + llama.cpp + Piper) läuft komplett auf
Deutsch. Direkter Qualitätsvergleich gegen OpenAI wird möglich.

## Modell-Swaps
- **STT:** `ggml-small.en-q5_1.bin` (190 MB, EN-only) → zusätzlich
  multilinguales `ggml-small-q5_1.bin` (~184 MB) registrieren.
  Quelle: `https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small-q5_1.bin`
  — SHA256 vor PR von HuggingFace verifizieren.
- **TTS:** `en_US-lessac-medium.onnx` (63 MB) → zusätzlich
  `de_DE-thorsten-medium.onnx` (~63 MB) aus `rhasspy/piper-voices`.
  Inklusive `.onnx.json` Config-Datei.
- **LM:** Llama 3.2 3B Instruct Q4_K_M ist bereits multilingual —
  **kein Modell-Swap nötig**, nur deutscher System-Prompt (siehe M5,
  derselbe `gpt_classify_prompt_de` wird auf Local wiederverwendet).

## Kritische Dateien
- `src/persistence/model_manifest.cpp:37-117` — zwei neue Manifest-
  Einträge (multilingual Whisper, German Piper Voice) mit URLs,
  SHA256-Hashes, Größen. Existierendes Manifest-Schema reicht aus.
- `src/backends/loader.cpp` — bei Local-Mode + DE-Region die DE-Modelle
  laden statt der EN-Modelle. Modell-Pfad-Resolution geht heute über
  `model_paths` — ggf. um Sprach-Suffix erweitern oder eigenständigen
  DE-Manifest-Pfad nutzen.
- `src/backends/whisper_stt.cpp:59` — `wparams.language = "en"`
  hardcoded → aus `backend_language()` lesen.
- `src/backends/piper_tts.cpp` — Voice-File-Pfad aus Settings/Region,
  nicht hardcoded.
- `src/backends/llama_lm.cpp` — System-Prompt aus
  `gpt_classify_prompt_de` wenn DE.
- `src/ui/atc_ui.cpp` — Models-Tab muss die DE-Modelle anzeigen +
  Download-Buttons. Bei Region-Switch automatisch Modell-Anforderung
  neu evaluieren.

## Risiko
Llama 3.2 3B Q4_K_M auf Deutsch ist für Intent-Klassifikation qualitativ
ungewiss. Mitigation: Der LM-Pfad wird nur bei low-confidence-Intent
(< 0.7) angesprochen — high-confidence-Intents gehen weiterhin direkt
durch den Rule-Parser (M7). Falls LM-DE schwächelt, ist der Schaden
begrenzt.

## Erwarteter Qualitäts-Vorteil gegenüber OpenAI
Piper `de_DE-thorsten-medium` ist nativ deutsch trainiert und spricht
NATO-Buchstaben (Alfa, Bravo, Charlie, ..., Zulu) und Zahlen mit
deutscher Sprachfärbung aus ("Tschar-li", "Wis-ki", "Tsu-lu"). OpenAI
tts-1 verwendet englisch trainierte Stimmen und spricht dieselben
Buchstaben mit englisch-amerikanischem Akzent ("Tshaar-lee", "Wis-kee",
"Zoo-loo"). Für deutsche BZF-Funkohren ist das ein hörbarer
Authentizitäts-Unterschied → explizit als DoD-Vergleichspunkt notieren.

## DoD
Local-Backend + DE-Region läuft eine Platzrunde end-to-end auf Deutsch
auf M-Series Mac. Modell-Downloads in `Models`-Tab funktionieren inkl.
SHA256-Verify. A/B-Test gegen OpenAI: NATO-Buchstaben-Aussprache und
"zwo"-Zahlworte werden subjektiv bewertet.
