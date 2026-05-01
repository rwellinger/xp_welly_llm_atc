### AI-powered ATC voice communication plugin for X-Plane 12 VFR flights

**Apple Silicon only.** Built for macOS users on M-series Macs who lack
access to commercial ATC voice solutions on their platform. Runs **fully
offline** once the models are downloaded — no cloud, no API key, no
recurring cost.


### What's New in vX.Y.Z (TBD) — Local-Inference Migration

This is a significant release: the cloud-based STT / LLM / TTS pipeline
has been replaced with fully local inference on Apple Silicon. The ATC
state machine, intent parser, ATIS generator, and UI structure are
unchanged.

  - **Speech-to-text now runs locally** via `whisper.cpp` (Metal-accelerated),
    using `ggml-small.en-q5_1` (181 MB).
  - **Intent disambiguation now runs locally** via `llama.cpp` (Metal-accelerated),
    using `Llama-3.2-3B-Instruct-Q4_K_M` (1.88 GB). Triggered when the
    rule-based parser is uncertain (confidence < 0.7) or for sub-variant
    distinctions like pattern-vs-cross-country departure.
  - **Text-to-speech now runs locally** via Piper, using `en_US-lessac-medium`
    (60 MB). All ATC speakers share one voice; ATIS speaks slower via
    Piper's `length_scale=1.18`.
  - **No more OpenAI dependency.** No API key, no Keychain entry, no HTTPS
    traffic to OpenAI servers. The plugin is now usable in offline / air-gapped
    environments after a one-time model download.
  - **In-plugin model downloader.** A new **Models** tab in the ImGui UI shows
    per-file state (Missing / Downloading / Verifying / Ready). Click
    *Download all missing* to pull ~2.0 GB from HuggingFace over HTTPS.
    Resumable (HTTP `Range`), cancellable, SHA256-verified before use.
    `.part` files survive cancel + Wi-Fi drops; only the verified file is
    renamed to its final name.
  - **Performance** (warm, M4): STT 321 ms · LM 634 ms · TTS 200 ms ·
    **total ≈ 1.16 s per request**. Spike validated end-to-end pipeline
    in `spikes/spike_e2e/RESULTS.md`.
  - **Settings JSON simplified.** Removed: `api_key_saved`, `tts_voice_*`,
    `tts_model`, `whisper_model`, `gpt_model`, `gpt_fallback_enabled`.
    The plugin migrates legacy `settings.json` files automatically — old
    keys are stripped on first load.
  - **PTT hard-gate.** Push-to-talk is disabled until all three backends
    are loaded. The Status tab surfaces a banner pointing to the Models
    tab; the Models tab label shows `(!)` in amber when something is
    missing.
  - **Plugin path resolution.** Models live under
    `<plugin>/Resources/models/` resolved via `XPLMGetPluginInfo` —
    portable across X-Plane installs on internal SSD, external
    USB/Thunderbolt drives, and volume paths with spaces or non-ASCII
    characters. No reliance on `$HOME` or CWD.
  - **Architecture: arm64 only.** The plugin no longer ships an x86_64
    slice. See *Requirements* below for what this means for Intel-Mac
    users.


### Features

  - Push-to-Talk with **local** speech recognition (`whisper.cpp` + Metal)
  - Rule-based ATC state machine for towered and non-towered airports
  - **Local LLM** intent classification (`llama.cpp` + Metal, Llama 3.2 3B)
    for low-confidence transcripts and pattern-vs-cross-country
    disambiguation
  - **Local** ATC voice responses (Piper, en_US-lessac-medium); ATIS
    speaks slower
  - Full VFR traffic pattern: taxi, takeoff, pattern, landing
  - Cross-country flights with active ATC handoffs (Tower → Departure/FIS
    → Approach → Tower) driven by X-Plane airspace polygons
  - "On course" departure detection, VRP-based inbound at destination
  - Touch-and-go and go-around procedures
  - ATIS generation from live sim weather
  - Automatic runway selection based on wind
  - Frequency-aware state management (Ground / Tower / Approach / FIS / ATIS / UNICOM)
  - Per-airport/runway pattern direction and Visual Reporting Points (VRPs)
  - Context-aware Phraseology Hints with ICAO phraseology tooltips
  - "Disregard" button for recovering from stuck ATC conversations
  - Radio power detection with configurable bypass
  - Configurable ATC recovery timing for beginners and experienced pilots
  - Optional "Disable Default X-Plane ATC" to avoid double audio
  - **Models tab** — per-file state, progress bars, RAM resident size,
    per-stage inference latency for tuning
  - ImGui in-sim UI with frequency management, phraseology hints,
    transcript panel, and settings


### Installation

1. Download `xp_wellys_atc-vX.Y.Z-arm64.zip` from the GitHub Releases page.
2. Extract into `X-Plane 12/Resources/plugins/`.
3. Launch X-Plane. Open *Plugins → Welly's ATC*.
4. **First launch:** the **Models** tab shows three rows in red. Click
   **Download all missing** — ~2.0 GB pulled from HuggingFace, resumable.
5. Once all three rows are **Ready**, the PTT-disabled banner clears and
   the plugin is ready.

The bundle lays out as:

```
xp_wellys_atc/
├── mac_x64/
│     ├── xp_wellys_atc.xpl                ← arm64 only
│     ├── libpiper.dylib                   ← Piper TTS
│     ├── libonnxruntime.1.22.0.dylib      ← Piper's inference runtime
│     └── libonnxruntime.dylib             ← symlink-style alias
├── Resources/
│     ├── espeak-ng-data/                  ← Piper phonemizer data (~19 MB, bundled)
│     └── models/                          ← downloaded on first run
└── data/
      ├── settings.json
      ├── atc_prompt_templates.json
      └── regions/{eu,us}/*.json
```


### Requirements

  - **macOS 13.3 or later** (onnxruntime 1.22.0 prebuilt arm64 dylib
    is built against the macOS 13.3 SDK)
  - **Apple Silicon Mac (M1 / M2 / M3 / M4).** Intel Macs are **not
    supported** — see below.
  - X-Plane 12
  - 32 GB RAM recommended (X-Plane uses 8–16 GB; ~3 GB headroom for
    the inference stack)
  - ~2.5 GB free disk on whichever volume hosts the plugin (models +
    bundle)
  - Internet on first launch only, to download the models. After that
    the plugin runs fully offline.

#### Intel Mac users

The plugin ships **arm64-only**. On an Intel Mac (or under Rosetta), X-Plane
silently skips the load — the only trace is a single line in
`X-Plane 12/Log.txt`:

> *Couldn't load plugin .../xp_wellys_atc.xpl: bad architecture*

There is no in-sim error. Verify your Mac's architecture before installing:

```sh
uname -m   # must print "arm64"
```

Intel Macs are not supported by this plugin.


### Known Limitations

  - **Apple Silicon only** — onnxruntime / Metal / pipeline budget all
    assume ARM64
  - Single pilot — no traffic sequencing, always "number one"
  - English only — non-English ATC not supported
  - EU/ICAO and US/Canada phraseology supported; other variants
    (UK CAA-specific, Australian AIP, etc.) fall back to ICAO defaults
  - Hardcoded taxiway — taxi instructions always use "via Alpha"
  - Single TTS voice for all speakers (Tower, Ground, ATIS share
    `en_US-lessac-medium`; ATIS distinguished only by `length_scale=1.18`
    for slower speech)
  - Multi-leg handoffs require X-Plane 12 atc.dat (ships by default;
    gracefully disabled if missing)


### Roadmap

  - Traffic support over LiveTraffic Plugin
  - Additional airports with VRPs and pattern directions
  - VFR Reporting Points beyond the currently shipped Swiss / German set
  - IFR Support — clearances, holds, approach procedures (later phase)


### Migration From the Earlier Cloud Build

If you are upgrading from a previous OpenAI/cloud-based `xp_wellys_atc`
install:

  - The OpenAI API key in your macOS Keychain is no longer used. The
    plugin no longer reads, writes, or removes it. You can delete it
    manually if you wish (Keychain Access → search `xp_wellys_atc`).
  - Your existing `settings.json` is migrated automatically. Legacy
    keys (`api_key_saved`, `tts_voice_*`, `tts_model`, `whisper_model`,
    `gpt_model`, `gpt_fallback_enabled`) are stripped on first load.
  - The traffic-pattern direction, callsign, region, and other
    non-cloud settings are preserved.
  - First launch will trigger the model download dialog.


  Full Changelog: https://github.com/rwellinger/xp_welly_llm_atc/compare/v1.5.1...vX.Y.Z
