# Milestone 06 — Plugin Integration (Strategy Pattern)

## Goal

Replace the OpenAI-based STT/LLM/TTS backends in the X-Plane plugin with the locally
running implementations validated in milestones 02–05. The ATC state machine, flight
phase detection, ATIS generation, and ImGui UI structure stay functionally unchanged.
**Only enter this milestone after the spike is officially "go"** (per milestone 05).

## Deliverables

1. **Refactored backend layer** based on the Strategy interfaces designed in milestone 01:
   - `ISpeechToText`, `ILanguageModel`, `ITextToSpeech` (or whatever names were agreed).
   - Concrete `WhisperCppStt`, `LlamaCppLlm`, `PiperTts` implementations sourced from
     the spike code.
   - The old OpenAI implementations are **removed**, not kept behind a flag — the fork
     is dedicated to the local stack.
2. **Static linking** of whisper.cpp, llama.cpp, Piper, and onnxruntime into the plugin
   `.xpl` binary. Single deliverable, no dylib hunt at runtime.
3. **Model loader** in the plugin:
   - Looks for models under `Resources/models/` **relative to the plugin's own
     location**, derived via `XPLMGetPluginInfo` + `XPLMGetSystemPath`. Never
     `$HOME`, `~/Library`, or CWD. This makes the plugin portable across
     X-Plane installations on internal SSDs, external USB/Thunderbolt drives,
     and volume paths containing spaces or non-ASCII characters.
   - Loads on plugin startup (not on first inference) — predictable cold-start.
   - Surfaces load errors via the existing logging path.
4. **In-plugin model downloader**:
   - On plugin startup, scan `Resources/models/` for the expected model files
     (whisper, llama, piper) and SHA256-verify each against the bundled
     manifest.
   - If any are missing or mismatch, present an ImGui dialog with a per-model
     "Download now" button. No silent background download — the user opts in.
   - libcurl on a dedicated `std::thread`, streaming-write directly to the
     final destination on whichever disk hosts the plugin (no temp roundtrip
     via the system disk — important for users on small internal SSDs with
     X-Plane on an external drive).
   - HTTP `Range` resume support (`CURLOPT_RESUME_FROM_LARGE`) so that a
     dropped USB/Thunderbolt link or Wi-Fi blip does not restart a 2 GB
     download from scratch.
   - Available-disk-space precheck (~2.3 GB total budget for all three models)
     using `std::filesystem::space()` on the target dir before starting.
   - Progress reported via `std::atomic<size_t>` read once per ImGui frame —
     no locking on the render path.
   - Cancel flag (`std::atomic<bool>`) checked in libcurl's progress
     callback; the worker exits within ~1 s of cancel.
   - `.part` suffix during download; atomic rename to the final filename
     **only** after SHA256 verification succeeds. A corrupted partial file
     can never be loaded as a model.
   - On `XPluginDisable`: set cancel flag, then `join()` the worker thread
     before returning. No threads survive the .xpl unload.
5. **ImGui status panel** addition:
   - Per-backend state: `Missing / Downloading <pct>% / Verifying / Ready / Error`.
   - Per-model: filename, expected size, SHA256, current state, action button
     (Download / Cancel / Re-verify / Retry).
   - RAM consumption (sum of resident sizes, polled every few seconds).
   - Last inference latency per stage (for live tuning).
6. **README updates**:
   - Hardware requirements (M1 / 32 GB).
   - Model download flow: "first launch shows a download dialog; ~2.3 GB total
     over HTTPS from HuggingFace; resumable; expect 5-30 min on typical home
     internet". Include the URLs/hashes as a manual-fallback table for users
     behind restrictive networks.
   - Build instructions (CMake, Xcode CLT, submodule init).
7. **Smoke test**: load X-Plane on a fresh install (no models present),
   trigger the download from the plugin UI, then fly a short scripted
   scenario and confirm a clearance request → controller response cycle works
   end-to-end inside the sim. Repeat with X-Plane installed on an external
   USB-SSD to validate the path-resolution requirement.

## Acceptance criteria

| Item | Target |
|------|--------|
| Plugin builds cleanly with `cmake --build build --target xp_welly_llm_atc` | yes |
| Plugin loads in X-Plane 12 on M1 without crash | yes |
| One full taxi/takeoff/cruise/landing scenario runs without backend errors | yes |
| End-to-end latency inside the sim matches milestone 05 numbers ±20% | yes |
| ATC state machine logic is functionally equivalent to the original | yes (manual diff review) |
| No OpenAI / network calls remain | verified by running offline |

## Architecture constraints (re-stated)

- ATC state machine: **untouched** beyond the call sites that now hit local backends
  through the Strategy interfaces.
- No background daemons, no helper processes, no Ollama, no external Python.
- Metal backend enabled at build time and runtime-verified.
- All third-party code: MIT or compatible (Apache-2.0, BSD). Document in `THIRD_PARTY.md`.

## Open decisions

- **[DECISION] — resolved 2026-04-30** Where do model files live in distribution?
  Options considered:
  (a) committed under Git LFS — easy install, but bloats the repo and pushes
      the LFS bandwidth cost onto whoever forks.
  (b) downloaded on first run by the plugin — needs HTTP code, but libcurl
      and ImGui are already in the build; adds ~200 LOC.
  (c) manual download per README — simplest, but requires Terminal use,
      which is poor UX for an X-Plane plugin.
  → **Decision: (b).** The plugin already does HTTP for OpenAI today; the
  same `std::thread` pattern (CLAUDE.md: *"All network/audio calls on
  std::thread"*) applies to model fetch. The user-experience win is large
  (single click vs. Terminal session), and SHA256 verification can live next
  to the download code instead of in shell snippets the user has to copy.
  Manual URLs/hashes still live in the README as a fallback for users behind
  restrictive networks. See deliverable 4 for concrete requirements.
- **[DECISION]** Threading model: dedicated worker threads per backend, or a single
  inference thread with a queue? → propose during this milestone after profiling.
- **[DECISION]** Behavior when a model fails to load: hard-fail the plugin, or run with
  the broken backend disabled and a UI warning? → likely hard-fail for the spike — user
  to confirm.

## Dependencies

- All previous milestones complete.
- Milestone 05 latency goal met → "go" recommendation in the README.

## Out of scope (for the spike phase)

- Multi-mirror / CDN fallback. HuggingFace is the single source; if it is
  down, the manual URL/hash table in the README is the user's fallback.
- Background / silent downloads. The download is always opt-in via the
  ImGui dialog.
- Differential / patch updates of model files. A model version bump is a
  full re-download.
- Multi-language support.
- Voice cloning, custom voices.
- Wake-word detection.
- Production-grade error recovery (graceful degradation when a model crashes mid-flight).
- Universal binary (x86_64 build) — Apple Silicon only.
