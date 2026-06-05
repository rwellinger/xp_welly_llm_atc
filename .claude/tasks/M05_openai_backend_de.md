# M5 — OpenAI-Backend Deutsch-fähig

**Plan-Referenz:** `~/.claude/plans/kannst-du-mir-einen-jolly-flamingo.md`
**Voraussetzung:** M2 (Templates), M3 (Normalizer)
**Folge-Milestones:** M6 (Local nutzt denselben System-Prompt-Key)

## Ziel
OpenAI-Pipeline (Whisper → gpt-4o-mini → tts-1) versteht und spricht
Deutsch korrekt.

## Kritische Dateien
- `src/backends/openai_stt.cpp:70-71` — Sprachparameter aus Settings
  oder Region ableiten statt hardcoded `"en"`. Vorschlag: neuer
  Helper `settings::backend_language()` der aus `flow_region`
  ableitet (`DE` → `"de"`, sonst `"en"`). Keine neue Settings-Spalte
  nötig, solange Sprach-Scope = "rein deutsch via DE-Region".
- `data/atc_prompt_templates.json` — neue Keys `gpt_classify_prompt_de`
  und `gpt_fallback_prompt_de` mit deutscher Anweisung
  ("Du bist ein ATC-Intent-Klassifikator..."). Bestehende Keys bleiben
  als EN-Default.
- `src/backends/openai_lm.cpp` — Prompt-Template-Auswahl nach
  `backend_language()`.
- `src/backends/openai_tts.cpp:66-77` — Stimm-Mapping evaluieren:
  OpenAI tts-1-Stimmen (`onyx`, `echo`, `alloy`, `nova`, `shimmer`)
  sind multilingual und sprechen Deutsch mit leichtem Akzent. Per
  A/B-Test die für deutschen ATC-Sound am besten passende Voice pro
  Rolle (ATIS / Tower / Ground) in den `settings.json`-Defaults setzen.
  Kein Sprachparameter zur API nötig — Voice IDs reichen.

## Bekannte Limitierung
OpenAI tts-1 Stimmen sind englisch trainiert. NATO-Buchstaben und
Zahlwörter klingen mit US-englischem Akzent ("Tshaar-lee" statt
"Tschar-li"). Für deutsche BZF-Realismus ist das fremd — wird in M6
gegen Piper de_DE-thorsten verglichen.

## DoD
Whisper transkribiert deutsche Funksprüche korrekt. Tower antwortet
auf Deutsch. Quality-Note in README dokumentiert (Akzent-Limitierung
der OpenAI-Stimmen).
