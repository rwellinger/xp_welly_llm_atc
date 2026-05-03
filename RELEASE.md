### AI-powered ATC voice communication plugin for X-Plane 12 VFR flights

**Apple Silicon only.** Built for macOS users on M-series Macs (M1 / M2 /
M3 / M4 — all natively, **no Rosetta**). Runs **fully offline** once the
models are downloaded — no cloud, no API key, no recurring cost.

> **Heads-up — Intel-Mac and Rosetta users**
> The local-LLM build (v2.x) is Apple-Silicon-only by design. The
> previous OpenAI-cloud build (**v1.5.3**, the last 1.x release) still
> works on Intel Macs and is preserved on the GitHub Releases page —
> but it requires your own OpenAI API key, which is the main reason
> the project moved to local inference. v1.5.x will not receive
> further features; bug-fix releases only on a strictly-needed basis.


### Training Videos

* Training Video 1: Configuration
* Training Video 2: ATIS and Ground Call for Taxi
* Training Video 3 (TBD): Cross-country VFR with traffic advisories *(coming)*


### License and Costs

This plugin is free, open source (GPL-3.0-or-later), and **costs nothing
to run** on v2.x. All inference happens locally on your Apple-Silicon
Mac via `whisper.cpp`, `llama.cpp`, and Piper. There is no API key, no
subscription, no per-request charge.

The one-time cost is disk space for the model files (~2.0 GB downloaded
on first launch from HuggingFace) and the time it takes to pull them
(5–30 minutes on typical home internet).


### Motivation

If you fly on Windows, you may already know **SayIntention.ai** — a
commercial product that lets you talk to ATC naturally and get realistic
responses. Great experience. Unfortunately, Windows-only.

Mac users are left with X-Plane's built-in ATC, which gets the job done
but lacks the immersion of real voice communication. There are ways to
use SayIntention on Mac via virtual machines and bridges, but they are
complicated and unstable.

This plugin was built to close that gap. The goal: bring a SayIntention-
style ATC experience to macOS — free, open source, and built specifically
for the platform. Currently focused on VFR flights, with IFR support
potentially on the roadmap.


### What's New in v2.1.0 — Traffic Awareness, ATIS & Phraseology

Significant feature drop on top of v2.0.0's local-inference foundation.
The pipeline (Whisper → Llama → Piper), state machine, and UI structure
remain unchanged — this release focuses on **what the plugin can hear,
say, and react to**.

#### Traffic Awareness (Phases 1 + 2)

  - **Live traffic context** — the plugin now reads X-Plane's standard
    `sim/cockpit2/tcas/targets/...` DataRefs at 2 Hz and maintains a
    `TrafficContext` snapshot of every aircraft within 40 NM. Works with
    **LiveTraffic, X-IvAp, swift, XSquawkBox, or native AI** — no
    provider-specific dependency, no `XPLMAcquirePlanes` override.
  - **VFR en-route traffic advisories** — when you are in established
    ATC contact (Tower / Approach / FIS) and another aircraft enters a
    forward conflict cone (2–8 NM, ±1500 ft, closing), the controller
    proactively calls the traffic in proper EU/ICAO phraseology:
    > *"Hotel Bravo X-ray Yankee Zulu, traffic, 2 o'clock, 3 miles,
    > opposite direction, indicating 4500 feet, type unknown."*
  - **Voice acknowledgement** — reply by voice with `"Traffic in sight"`,
    `"Negative contact"`, or `"Looking"`. The controller follows up
    appropriately ("roger, maintain visual separation" / "traffic now
    1 o'clock, 2 miles") without disturbing your main ATC flow. Visual
    acknowledgement applies a 5-minute lockout against re-advising the
    same target.
  - **Side-channel architecture** — traffic dialog runs *parallel* to
    the main ATC state machine. A taxi or landing readback is never
    interrupted by an advisory, and dismissing an advisory never
    breaks your main flow.
  - **Debug Traffic tab** — optional ImGui tab (toggled via
    `debug_traffic` setting) lists the 10 nearest aircraft with
    callsign, bearing, clock position, distance, altitude difference,
    groundspeed, and detected aircraft type (e.g. `C172`, `PA28`).
  - **Traffic injection for testing** — headless `atc_repl` accepts
    `--traffic-fixture <file.json>` for deterministic regression
    scenarios without needing X-Plane running.

#### ATIS Improvements

  - **ATIS now plays on COM1 *or* COM2** — pilots commonly park ATIS
    on the standby radio while keeping the active COM on Tower. Both
    workflows now work; ATIS plays through whichever COM bus is tuned
    to it.
  - **Mid-broadcast abort** — retuning the COM that's playing ATIS now
    immediately cuts the audio stream. No more ATIS droning on after
    you've already switched to Tower.
  - **Cooldown extended to 120 s** — eliminates duplicate ATIS playback
    during the typical ATIS → Approach → Tower frequency sequence on
    arrival.
  - **Stable ATIS letter** — already in v2.0.0 but worth re-stating:
    the letter advances only on real changes (active runway, wind
    direction > 30°, QNH > 1 hPa, visibility category), not on calm-
    wind jitter.

#### Communication & Understanding Improvements

A real-flight test (LSZG → LSZB) surfaced several rough edges in how
the plugin disambiguates pilot speech and picks ATC responses. All of
them are now fixed:

  - **Post-landing departure-restart bug fixed** — the closing readback
    *"general aviation parking via Alpha, good day"* could be
    misclassified as a fresh `REQUEST_TAXI` and trigger a new departure
    cycle (taxi instructions, takeoff briefing) right after the pilot
    parked. The readback safety net is now confidence-based and
    independent of `readback_pending`, so high-confidence readbacks
    always win over LM hallucinations.
  - **LM repair hallucinations rejected** — the local Llama 3.2 3B
    occasionally invented runway numbers, frequencies, or altitudes
    when "repairing" Whisper transcripts. A deterministic validator
    now discards repairs that introduce digit tokens absent from the
    pilot's original audio. The classifier prompt was also tightened
    with explicit "NEVER add details not in input" guidance and a
    counter-example.
  - **Clearance-readback patterns extended** — *"cleared into the
    control zone"*, *"joining instructions"*, *"cleared to enter"*, and
    Whisper's hyphenated *"control-zone"* variant are now recognised as
    READBACK and no longer dropped to UNKNOWN (which previously
    triggered LM mis-classification as `REPORT_POSITION_DOWNWIND`).
  - **Tower-only inbound clearance no longer redundant** — at airports
    where Tower handles all traffic (e.g. LSZB), the inbound clearance
    used to end with "contact Tower on 121.025" even when the pilot
    was already on that frequency. Templates now use a smart
    `{tower_handoff_phrase}` variable that suppresses the redundant
    handoff when the pilot is already on Tower.
  - **Approach controller more substantive** — first contact with
    Approach now responds with *"radar contact, runway X, QNH Y,
    report field in sight"* instead of just *"pass your message"*.
    Subsequent position reports proactively hand the pilot off to
    Tower with the right frequency.
  - **Aircraft registration shown in plugin UI** — the cockpit's actual
    tail number is read live from X-Plane and displayed in the ATC
    panel, making it easier to match the pilot callsign to what the
    sim shows on the aircraft.

#### Stability & Performance

  - **Crash fixes** — two crashes (one in the audio render path under
    rapid frequency changes, one in traffic context updates with empty
    target lists) traced and patched.
  - **Build cache** — `ccache` is auto-detected during configure;
    incremental rebuilds drop to seconds.
  - **Domain-matched traffic types** — traffic advisories now read
    "Cessna" / "PA28" / "Heavy" instead of always "type unknown" when
    the provider populates the ICAO type field (LiveTraffic does).
  - **Better quality gate on Whisper** — transmissions below 0.3
    quality are immediately rejected with a polite "say again" instead
    of being routed through the full pipeline.


### What's New in v2.0.0 — Local-Inference Migration

This was a significant release: the cloud-based STT / LLM / TTS pipeline
was replaced with fully local inference on Apple Silicon. The ATC state
machine, intent parser, ATIS generator, and UI structure are unchanged.

  - **Speech-to-text now runs locally** via `whisper.cpp`
    (Metal-accelerated), using `ggml-small.en-q5_1` (181 MB).
  - **Intent disambiguation now runs locally** via `llama.cpp`
    (Metal-accelerated), using `Llama-3.2-3B-Instruct-Q4_K_M` (1.88 GB).
    Triggered when the rule-based parser is uncertain (confidence < 0.7)
    or for sub-variant distinctions like pattern-vs-cross-country
    departure.
  - **Text-to-speech now runs locally** via Piper, using
    `en_US-lessac-medium` (60 MB). All ATC speakers share one voice;
    ATIS speaks slower via Piper's `length_scale=1.18`.
  - **No more OpenAI dependency.** No API key, no Keychain entry, no
    HTTPS traffic to OpenAI servers. The plugin is now usable in
    offline / air-gapped environments after a one-time model download.
  - **In-plugin model downloader.** A new **Models** tab in the ImGui UI
    shows per-file state (Missing / Downloading / Verifying / Ready).
    Click *Download all missing* to pull ~2.0 GB from HuggingFace over
    HTTPS. Resumable (HTTP `Range`), cancellable, SHA256-verified before
    use. `.part` files survive cancel + Wi-Fi drops; only the verified
    file is renamed to its final name.
  - **Performance** (warm, M4): STT 321 ms · LM 634 ms · TTS 200 ms ·
    **total ≈ 1.16 s per request**.
  - **Settings JSON simplified.** Removed: `api_key_saved`,
    `tts_voice_*`, `tts_model`, `whisper_model`, `gpt_model`,
    `gpt_fallback_enabled`. Legacy `settings.json` files are migrated
    automatically — old keys are stripped on first load.
  - **PTT hard-gate.** Push-to-talk is disabled until all three backends
    are loaded. Status tab surfaces a banner pointing to the Models tab.
  - **Plugin path resolution.** Models live under
    `<plugin>/Resources/models/` resolved via `XPLMGetPluginInfo` —
    portable across X-Plane installs on internal SSD, external
    USB/Thunderbolt drives, and volume paths with spaces or non-ASCII
    characters.
  - **Architecture: arm64 only.** The plugin no longer ships an x86_64
    slice. **Does not work when X-Plane 12 runs under Rosetta!**


### Features

  - Push-to-Talk with **local** speech recognition (`whisper.cpp` + Metal)
  - Rule-based ATC state machine for towered and non-towered airports
  - **Local LLM** intent classification (`llama.cpp` + Metal,
    Llama 3.2 3B) for low-confidence transcripts and pattern-vs-
    cross-country disambiguation
  - **Local** ATC voice responses (Piper, en_US-lessac-medium); ATIS
    speaks slower
  - **Live traffic awareness** — VFR en-route advisories with voice ack
    (Phase 1 + 2). Side-channel architecture leaves the main ATC flow
    intact.
  - Full VFR traffic pattern: taxi, takeoff, pattern, landing
  - Cross-country flights with active ATC handoffs (Tower → Departure /
    FIS → Approach → Tower) driven by X-Plane airspace polygons
  - "On course" departure detection, VRP-based inbound at destination
  - Touch-and-go and go-around procedures
  - **ATIS** generation from live sim weather, on COM1 or COM2,
    with mid-broadcast abort on retune
  - Automatic runway selection based on wind
  - Frequency-aware state management
    (Ground / Tower / Approach / FIS / ATIS / UNICOM)
  - Per-airport/runway pattern direction and Visual Reporting Points
    (VRPs)
  - Context-aware Phraseology Hints with ICAO phraseology tooltips
  - **"Disregard"** button — flow-aware recovery from stuck ATC dialogs
    (returns to PATTERN_ENTRY airborne / EN_ROUTE in transit / IDLE on
    ground)
  - Radio power detection with configurable bypass
  - Configurable ATC recovery timing for beginners and experienced pilots
  - Optional "Disable Default X-Plane ATC" to avoid double audio
  - **Models tab** — per-file state, progress bars, RAM resident size,
    per-stage inference latency for tuning
  - **Debug Traffic tab** (optional) — live list of nearby aircraft
  - **Aircraft registration display** — pilot callsign linked to the
    cockpit tail number
  - ImGui in-sim UI with frequency management, phraseology hints,
    transcript panel, and settings


### Installation

1. Download `xp_wellys_atc-v2.1.0-arm64.zip` from the GitHub Releases page.
2. Extract into `X-Plane 12/Resources/plugins/`.
3. Launch X-Plane. Open *Plugins → Welly's ATC*.
4. **First launch:** the **Models** tab shows three rows in red. Click
   **Download all missing** — ~2.0 GB pulled from HuggingFace, resumable.
5. Once all three rows are **Ready**, the PTT-disabled banner clears
   and the plugin is ready.
6. Bind a key or joystick button to the X-Plane command
   `xp_wellys_atc/ptt` (Settings → Keyboard / Joystick → search
   *xp_wellys_atc*).
7. In X-Plane's Sound tab, make sure the right input device (your
   headset's mic) is selected. The plugin uses X-Plane's audio
   configuration; no separate device selection.

The bundle lays out as:

```
xp_wellys_atc/
├── mac_x64/
│     ├── xp_wellys_atc.xpl                <- arm64 only
│     ├── libpiper.dylib                   <- Piper TTS
│     ├── libonnxruntime.1.22.0.dylib      <- Piper's inference runtime
│     └── libonnxruntime.dylib             <- symlink-style alias
├── Resources/
│     ├── espeak-ng-data/                  <- Piper phonemizer data (~19 MB, bundled)
│     └── models/                          <- downloaded on first run
└── data/
      ├── settings.json
      ├── atc_prompt_templates.json
      └── regions/{eu,us}/*.json
```


### Requirements

#### v2.x (this release — Apple-Silicon only)

  - **macOS 13.3 or later** (onnxruntime 1.22.0 prebuilt arm64 dylib
    is built against the macOS 13.3 SDK)
  - **Apple Silicon Mac (M1 / M2 / M3 / M4).** Intel Macs are
    **not supported.**
  - X-Plane 12 — **must run natively, NOT under Rosetta**
  - 32 GB RAM recommended (X-Plane uses 8–16 GB; budget ~3 GB headroom
    for the inference stack)
  - ~2.5 GB free disk on whichever volume hosts the plugin (models +
    bundle)
  - Internet on first launch only, to download the models. After that
    the plugin runs fully offline.
  - A headset with a microphone is recommended.

#### v1.5.3 (legacy OpenAI build — for Intel Macs)

  - macOS 12.0+ (ARM64 / x86_64 universal binary)
  - X-Plane 12
  - **Own OpenAI API key required** — see the v1.5.x release notes on
    GitHub for cost estimates and Keychain setup. Without it the plugin
    will not function.
  - A headset with a microphone is recommended.

#### Intel-Mac users

The v2.x plugin ships **arm64-only**. On an Intel Mac (or under Rosetta),
X-Plane silently skips the load — the only trace is a single line in
`X-Plane 12/Log.txt`:

> *Couldn't load plugin .../xp_wellys_atc.xpl: bad architecture*

There is no in-sim error. Verify your Mac's architecture before
installing:

```sh
uname -m   # must print "arm64"
```

Stay on **v1.5.3** (OpenAI cloud build) if you are on an Intel Mac.


### Known Limitations

  - **Apple Silicon only** (v2.x) — onnxruntime / Metal / pipeline
    budget all assume ARM64
  - Single pilot — no traffic *sequencing* yet (always "number one").
    Traffic advisories work, but the controller does not yet sequence
    multiple arrivals or give-way instructions on the ground.
  - English only — non-English ATC not supported
  - EU/ICAO and US/Canada phraseology supported; other variants
    (UK CAA-specific, Australian AIP, etc.) fall back to ICAO defaults
  - Hardcoded taxiway — taxi instructions always use "via Alpha"
  - Single TTS voice for all speakers (Tower, Ground, ATIS share
    `en_US-lessac-medium`; ATIS distinguished only by
    `length_scale=1.18` for slower speech)
  - Multi-leg handoffs require X-Plane 12 atc.dat (ships by default;
    gracefully disabled if missing)

Please note that the author is a hobby pilot, not a professional. Errors
in procedures or terminology are possible.


### Documentation

  - [Manual in Deutsch](docs/MANUAL_DE.md)
  - [Manual in English](docs/MANUAL_EN.md)
  - [Architecture analysis & spike results](docs/architecture-analysis.md)


### Support

Please report issues here:
[github.com/rwellinger/xp_welly_llm_atc/issues](https://github.com/rwellinger/xp_welly_llm_atc/issues).
Thank you.


### Contributing

Contributions are always welcome. If you are a developer interested in
improving the plugin, fork the repository and submit a Pull Request.
Please run `make lint` before submitting to keep the codebase clean and
consistent.

Some C++ and X-Plane 12 SDK skills are required. If you work with Claude
Code, the repository ships a `CLAUDE.md` and `.claude/` task specs that
make onboarding fast — start at
[github.com/rwellinger/xp_welly_llm_atc](https://github.com/rwellinger/xp_welly_llm_atc).


### Roadmap

  - **Traffic Phase 3** — ground / taxi conflict detection (
    *"hold position, traffic crossing"*, *"caution, aircraft taxiing
    on your left"*)
  - **Traffic Phase 4** — landing sequencing (*"number two, follow the
    Cessna on final"*)
  - **Traffic Phase 5** — takeoff sequencing (line-up-and-wait, hold
    short for landing traffic)
  - More VRPs for Swiss and other EU airports (you can add your own
    airports anytime via the config file)
  - IFR Support — clearances, holds, approach procedures (later phase)


### Related Plugins (by the same author)

  - **xp_pilot** — Cockpit assistant
  - **xp_wellys_atc** (legacy 1.5.x, OpenAI) — preserved for Intel Macs
  - **xp_swiss_vfr** — Swiss VFR procedures (in development)


### Migration From v1.5.x (Cloud Build)

If you are upgrading from a previous OpenAI-based `xp_wellys_atc` install:

  - The OpenAI API key in your macOS Keychain is no longer used. The
    plugin no longer reads, writes, or removes it. You can delete it
    manually if you wish (Keychain Access → search `xp_wellys_atc`).
  - Your existing `settings.json` is migrated automatically. Legacy
    keys (`api_key_saved`, `tts_voice_*`, `tts_model`, `whisper_model`,
    `gpt_model`, `gpt_fallback_enabled`) are stripped on first load.
  - Traffic-pattern direction, callsign, region, and other non-cloud
    settings are preserved.
  - First launch will trigger the model-download dialog.


Full Changelog: https://github.com/rwellinger/xp_welly_llm_atc/compare/v2.0.0...v2.1.0
