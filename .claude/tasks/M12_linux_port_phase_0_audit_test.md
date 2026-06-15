# M12 — Linux Port — Phase 0: MISTRAL Audit Invariant

## Context

Cross-platform port (macOS → macOS + Linux) on branch `feat/linux-port`. Linux is cloud-only (no whisper.cpp / llama.cpp / Piper), supports OpenAI **and** Mistral backends, ships as `experimental` for beta testers since the maintainer has no Linux hardware.

This is **Phase 0** — the safety net that must land before any other porting work. A community Linux fork (`/Users/robertw/Workspace/x-plane/linux/`) silently violated the Backend Adapter Rule by routing Mistral TTS through `OpenAiTts` with a Mistral URL. The audit test (`tests/test_audit_logging.cpp`) did not catch it because it only enforces LOCAL/OPENAI invariants today.

## Goal

Extend `tests/test_audit_logging.cpp` so the same source-level invariants that protect LOCAL and OPENAI backend boundaries also protect the MISTRAL family. After this phase, any future copy-paste from `openai_*.cpp` into `mistral_*.cpp` (or vice versa) fails CI.

## Steps

1. Read the existing test pattern in `tests/test_audit_logging.cpp` for the OPENAI checks.
2. Mirror those checks for `src/backends/mistral_stt.cpp`, `mistral_lm.cpp`, `mistral_tts.cpp`:
   - **MUST contain** their `[STT|LM|TTS]-MISTRAL` audit tag
   - **MUST NOT** `#include` `whisper.h`, `llama.h`, or `piper.h`
   - **MUST NOT** contain a `-LOCAL]` or `-OPENAI]` tag
   - **MUST NOT** reference `api.openai.com`
3. Tighten the existing OpenAI checks too: they must NOT reference `api.mistral.ai` or contain a `-MISTRAL]` tag (currently asymmetric).
4. Tighten the local-backend checks: they must NOT reference `api.mistral.ai` either (currently only OpenAI is forbidden).
5. Run `make test` on macOS — all three Mistral implementations currently in `main` must pass cleanly. If anything fails, fix the offending file before merging this phase.

## Files Affected

- `tests/test_audit_logging.cpp` — extend existing grep-style invariants

## CMake Changes

None.

## Commit Message

```
test: extend audit invariant to MISTRAL backend family

Mirror the LOCAL/OPENAI source-level grep checks for Mistral. Any future
copy-paste between cloud families now fails CI before merge.

Closes M12 Phase 0.
```

## Validation

- `make test` green on macOS
- Manually corrupt one Mistral file (e.g. add a stray `[TTS-OPENAI]` log) and confirm the test fails. Revert.

## Depends On

Nothing. This is the entry point of the port.

## Estimated Effort

~2 hours
