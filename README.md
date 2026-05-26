# Welly's ATC — Local AI Voice ATC for X-Plane 12

> **Fully local-inference X-Plane 12 ATC plugin for Apple Silicon.**
> `whisper.cpp` (Metal) + `llama.cpp` (Metal) + Piper TTS, bundled with the
> plugin. **No daemons, no helper apps, no cloud, no API keys.**
>
> The spike-phase architecture and per-backend measurements are archived in
> [`docs/architecture-analysis.md`](docs/architecture-analysis.md) and
> [`spikes/spike_e2e/RESULTS.md`](spikes/spike_e2e/RESULTS.md).
>
> **Measured pipeline latency** (warm, M4, end-to-end spike):
> STT 321 ms · LM 634 ms · TTS 200 ms · **total ≈ 1.16 s per request** —
> well under the 3 s acceptance target with > 1.8 s of headroom for the
> M4-vs-M1 generational gap and the plugin's main-thread / Core Audio
> overhead. M1 re-validation: pending real-flight smoke test.

---

AI-powered ATC voice communication plugin for X-Plane 12 VFR flights, running
fully offline once the models are downloaded.

Talk to ATC using your microphone via push-to-talk. The plugin transcribes your
speech locally with whisper.cpp, interprets your intent through a rule-based
ATC state machine (with low-confidence fallback to a local Llama 3.2 3B
classifier), and plays back ATC responses synthesised locally with Piper.

## Features

- **Push-to-Talk** — bound via X-Plane command binding (keyboard or joystick)
- **Local Speech-to-Text** — `whisper.cpp` `small.en-q5_1`, Metal-accelerated
- **Local LLM** — `llama.cpp` running Llama 3.2 3B Instruct (Q4_K_M),
  Metal-accelerated; used for intent disambiguation when the rule-based
  parser is uncertain. Repair output is digit-validated to suppress
  hallucinated runways or frequencies.
- **Local Text-to-Speech** — Piper, neutral US accent (`en_US-lessac-medium`),
  CPU + onnxruntime
- **ATC State Machine** — VFR phraseology for towered and non-towered airports
- **Flight Phase Detection** — context-aware guards prevent unrealistic ATC
  interactions based on aircraft state (parked, taxi, airborne, etc.)
- **Live Traffic Awareness (v2.1)** — provider-agnostic
  `sim/cockpit2/tcas/targets/...` reader feeding a 2 Hz `TrafficContext`
  snapshot. EU-phraseology en-route advisories with voice
  acknowledgement (`"Traffic in sight"` / `"Negative contact"` /
  `"Looking"`) on a side-channel that does not interfere with the main
  ATC flow.
- **ATIS Generation** — automatic ATIS broadcasts from live sim weather
  data, on COM1 *or* COM2 (active or standby). Aborts mid-broadcast if
  the pilot retunes the COM that's playing.
- **Radio discipline coaching** — ATC issues a polite reminder when the pilot
  uses inappropriate language, escalating on repeats
- **Phraseology Hints** — context-aware cheat sheet with full ICAO phraseology
  on hover
- **Cross-Country Support** — full VFR departure, en-route frequency change,
  and inbound flow between airports. Approach controller proactively
  hands off to Tower with the destination frequency.
- **Aircraft registration display** — pilot callsign linked to the
  cockpit's actual tail number read from X-Plane
- **"Disregard" recovery** — flow-aware reset (PATTERN_ENTRY when
  airborne near the home airport, EN_ROUTE in transit, IDLE on the
  ground)
- **Radio Power Awareness** — ATC panel disables when COM radio has no
  electrical power, with optional bypass for exotic aircraft
- **In-plugin model downloader** — first launch surfaces an ImGui dialog,
  HTTPS-resumable downloads from HuggingFace, SHA256-verified before use
- **ImGui UI** — in-sim ATC panel with frequency management, phraseology
  hints, transcript history, a Models tab for download / re-verify, and
  an optional Traffic tab (debug) listing the 10 nearest aircraft

## Hardware Requirements

| Item | Requirement |
|---|---|
| CPU | Apple Silicon (M1 / M2 / M3 / M4). **Intel Macs are not supported.** |
| RAM | 32 GB recommended (X-Plane 12 typically uses 8–16 GB; budget ~3 GB headroom for the inference stack) |
| Disk | ~2.5 GB free wherever the plugin is installed (models + bundle) |
| GPU | Any Metal-capable GPU on the same Apple Silicon chip |

The plugin builds and ships **arm64-only**. On an Intel Mac the `.xpl` is
silently skipped by X-Plane (a single line in `Log.txt`: *"Couldn't load
plugin … bad architecture"*) — there is no in-sim error. Verify your Mac's
architecture before installing:

```sh
uname -m   # must print "arm64"
```

## Software Requirements

| Item | Requirement |
|---|---|
| macOS | **13.3 or later** (onnxruntime 1.22.0 requires this; lower versions show a linker warning at build time and may crash at runtime) |
| X-Plane | X-Plane 12 (12.0 or later) |
| For building from source | CMake 3.26+, Homebrew LLVM (`brew install llvm`), Xcode Command Line Tools |

## Quick Start (pre-built release)

1. Download `xp_wellys_atc-vX.Y.Z-arm64.zip` from the GitHub Releases page.
2. Extract into `X-Plane 12/Resources/plugins/`. Result:
   ```
   X-Plane 12/Resources/plugins/xp_wellys_atc/
     ├── mac_x64/
     │     ├── xp_wellys_atc.xpl
     │     ├── libpiper.dylib
     │     ├── libonnxruntime.1.22.0.dylib
     │     └── libonnxruntime.dylib
     ├── Resources/
     │     └── espeak-ng-data/   (~19 MB, ships with the plugin)
     └── data/
           └── (templates, region rules, etc.)
   ```
3. Launch X-Plane. Open the plugin window via *Plugins → Welly's ATC*.
4. **First launch only**: the **Models** tab shows three rows in red. Click
   **Download all missing** — the plugin pulls ~2.0 GB from HuggingFace
   over HTTPS. Resumable; cancellable; SHA256-verified after each file.
5. Once all three rows show **Ready** (green), the PTT-disabled banner on
   the Status tab disappears and you can fly.

## Build From Source

```sh
git clone --recurse-submodules <repo-url>
cd xp_welly_llm_atc
make setup      # X-Plane SDK, Dear ImGui, nlohmann/json, Catch2
make build      # CMake Release build → build/xp_wellys_atc.xpl
                # builds whisper.cpp + llama.cpp + Piper from the spike
                # submodules; the first build is slow (~2 min on M4)
make install    # Code-sign + install to X-Plane plugins directory
```

The build downloads onnxruntime's prebuilt arm64 dylib (≈ 33 MB) into
`spikes/spike_piper/third_party/piper1-gpl/libpiper/lib/` on first
configure. After that everything is local.

## Local Inference Models

The plugin ships **without** the model files (~2.0 GB combined). They live
under `<plugin>/Resources/models/` and are downloaded on first launch via
the **Models** tab. Each download is HTTPS, resumable (`Range` header),
streamed directly to the install volume (no temp roundtrip via the system
disk — important for users running X-Plane on an external SSD), and
SHA256-verified before being renamed from `<file>.part` to its final
filename.

### Manual fallback (restrictive networks)

If the in-plugin downloader cannot reach HuggingFace (corporate proxy,
captive portal, etc.), download these files manually and drop them into
`<plugin>/Resources/models/`. The plugin re-verifies on the next launch
and loads them automatically if the hashes match.

| Model | Size | SHA256 | URL |
|---|---:|---|---|
| `ggml-small.en-q5_1.bin` | 181 MB | `bfdff4894dcb76bbf647d56263ea2a96645423f1669176f4844a1bf8e478ad30` | [`huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.en-q5_1.bin`](https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.en-q5_1.bin) |
| `Llama-3.2-3B-Instruct-Q4_K_M.gguf` | 1.88 GB | `6c1a2b41161032677be168d354123594c0e6e67d2b9227c84f296ad037c728ff` | [`huggingface.co/bartowski/Llama-3.2-3B-Instruct-GGUF/resolve/main/Llama-3.2-3B-Instruct-Q4_K_M.gguf`](https://huggingface.co/bartowski/Llama-3.2-3B-Instruct-GGUF/resolve/main/Llama-3.2-3B-Instruct-Q4_K_M.gguf) |
| `en_US-lessac-medium.onnx` | 60 MB | `5efe09e69902187827af646e1a6e9d269dee769f9877d17b16b1b46eeaaf019f` | [`huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/lessac/medium/en_US-lessac-medium.onnx`](https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/lessac/medium/en_US-lessac-medium.onnx) |
| `en_US-lessac-medium.onnx.json` | 4.9 KB | `efe19c417bed055f2d69908248c6ba650fa135bc868b0e6abb3da181dab690a0` | [`huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/lessac/medium/en_US-lessac-medium.onnx.json`](https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/lessac/medium/en_US-lessac-medium.onnx.json) |

After dropping the files in, reopen the plugin window — the Models tab
runs SHA256 verification in the background and flips the rows to **Ready**
once each hash matches.

### Expected first-run download time

5–30 minutes on typical home internet; the bottleneck is HuggingFace's
download throughput, not the plugin. The downloader resumes via HTTP
`Range` if your link drops, so a Wi-Fi blip mid-Llama does not restart
the 1.88 GB pull from scratch.

## Configuration

Settings live in `<plugin>/data/settings.json`. The local-inference
build ships with a slim schema — no API keys, no model selectors:

| Setting | Default | Description |
|---|---|---|
| `pilot_callsign` | *(empty)* | Phonetic callsign (set in plugin settings, written from the registration via ICAO conversion) |
| `active_com` | `1` | Active COM radio (1 or 2) |
| `volume` | `1.0` | Playback volume (0.0–1.0) |
| `pattern_direction` | `left` | Default traffic pattern direction (left/right) — overridden per airport/runway by `airport_vrps.json` |
| `disable_default_atc` | `false` | Suppress X-Plane's built-in default ATC |
| `skip_radio_power_check` | `false` | Bypass radio power detection (workaround for exotic aircraft) |
| `show_phraseology_hints` | `true` | Show phraseology cheat sheet in ATC panel |
| `auto_correction_factor` | `1.0` | ATC recovery time multiplier (0.5 = faster, 2.0 = slower) |
| `flow_region` | `EU` | `EU` (ICAO/QNH/hPa) or `US` (FAA-TC/altimeter/inHg) phraseology |
| `debug_logging` | `false` | Enable verbose debug output |
| `debug_traffic` | `false` | Show the Traffic tab in the ATC panel (lists the 10 nearest aircraft from the TCAS DataRefs) |

ATC response templates are defined in `data/regions/{eu,us}/atc_templates.json`.
Flight phase detection thresholds, ATC precondition guards, frequency guards,
and auto-correction rules are in `data/regions/{eu,us}/flight_rules.json`.
Switching the Region setting hot-reloads both files. All data files can be
edited without rebuilding the plugin.

### Airport Database (`data/airport_vrps.json`)

Per-airport configuration for Visual Reporting Points (VRPs) and traffic
pattern directions. Pre-populated for common Swiss and German VFR airports.
Each top-level key is an ICAO code with optional fields:

- `name` — display name
- `pattern_direction` — per-runway `"left"` / `"right"` (overrides the
  global `pattern_direction` setting)
- `vrps` — array of `{ name, lat, lon, alt_ft }`; `name` is the phonetic
  spelling (e.g. `"November"`) so Whisper and Piper handle it cleanly
- `arrival_routes` — per-runway ordered list of VRP names used for
  inbound routing

### ATC Response Templates (`data/regions/{eu,us}/atc_templates.json`)

Defines the ATC response text for every combination of airport type, ATC
state, and pilot intent. `towered` (full ATC flow) and `uncontrolled`
(CTAF/UNICOM self-announce) sections; each entry has `response`,
`next_state`, `requires_readback`. The special key `_INVALID` is the
fallback ("say again your request"). Variables are substituted from
`XPlaneContext` at runtime.

### Flight Rules (`data/regions/{eu,us}/flight_rules.json`)

Six sections — `phase_thresholds`, `hysteresis`, `intent_preconditions`,
`auto_corrections`, `intent_frequency`, `pilot_phraseology`.

### LLM Prompt Templates (`data/atc_prompt_templates.json`)

Prompts the engine sends to the local Llama 3.2 3B model:

| Key | Purpose |
|---|---|
| `whisper_prompt` | Initial-prompt hint for whisper.cpp to bias transcription toward aviation vocabulary and the NATO phonetic alphabet |
| `gpt_classify_prompt` | System prompt for low-confidence intent classification (variables: `{state}`, `{valid_intents}`, `{transcript}`, `{frequency_type}`, `{on_ground}`, `{altitude_ft}`, `{groundspeed_kts}`, `{airport}`) |

The key name keeps the `gpt_*` prefix for backwards compatibility with
existing `atc_prompt_templates.json` files; the local pipeline feeds
this prompt to Llama 3.2 unchanged.

**Push-to-Talk** is configured via X-Plane's keyboard or joystick settings.
The plugin registers the command `xp_wellys_atc/ptt` which can be bound to
any key or joystick button.

## Usage

1. Tune COM1/COM2 to the appropriate frequency in X-Plane (or click a
   frequency in the ATC panel to set it as standby, then flip-flop).
2. Hold the PTT key and speak your radio call — the **Phraseology Hints**
   panel shows you what to say (hover for full ICAO phraseology).
3. Release PTT — the plugin transcribes locally, processes through the
   state machine, and plays back the ATC response.
4. Check the ImGui overlay for transcript history and current ATC state.
5. If you get stuck in a loop, click the **Disregard** button to reset.

## Make Targets

```sh
make all        # clean + format + build + lint + test (full local CI)
make build      # build only
make test       # unit tests + scenario tests
make install    # code-sign + install to X-Plane
make repl       # build the headless atc_repl tool
make format     # clang-format
make lint       # clang-tidy (some rules promoted to errors)
make clean      # remove build/
make distclean  # also remove sdk/, vendor/
```

## Known Limitations

| Limitation | Impact | Effort |
|---|---|---|
| **Apple Silicon only** | Intel Macs cannot run the plugin | High — onnxruntime, Metal kernels, and pipeline budget all assume ARM64 |
| **English only** | non-English ATC not supported | Medium — would need different whisper / Llama / Piper voice |
| **Single-voice TTS** | All ATC speakers (Tower, Ground, ATIS) use the same Piper voice; ATIS speaks slower via `length_scale=1.18` | Low — could ship more voices and add a per-frequency selector |
| **"via Alpha" hardcoded** — taxiway name is always Alpha | Unrealistic at airports with different taxiway layouts | High — would need taxiway data from apt.dat or WED |
| **No traffic *sequencing*** — always "number one" — *traffic awareness* (advisories) shipped in v2.1; sequencing pending Phases 4 + 5 | Unrealistic at busy airports | Medium — Phases 4 + 5 on roadmap |
| **No callsign validation** | ATC accepts any callsign | Low priority for single-player |
| **Big-hub airports (LSZH, LSGG, …) not officially supported** — pilot can fly inbound/outbound, but Delivery (slot/VFR-clearance) workflow, RWY-specific Tower routing, and AIP VFR reporting points are not modelled | Generic hints at large hubs do not match real-world procedures (slot enforcement, multiple Tower frequencies, mandatory VFR points) | High — needs per-airport AIP research + new Delivery intent + slot setting + multi-Tower disambiguation |

## FAQ

**Does this support IFR or flight planning?**
Not today — the plugin is VFR-only. No IFR clearances, no flight-plan filing,
no FMS / route integration.

**Will there be a virtual co-pilot or checklist reader?**
Not in scope today. The plugin is a single-pilot Pilot ↔ ATC voice interface;
intercom and checklists are not implemented.

**Is it compatible with all XP12 aircraft and add-ons?**
Yes, in principle. The plugin is aircraft-agnostic and uses only standard
X-Plane DataRefs — no aircraft-specific code paths and no compatibility list.
It works with the default fleet (C172, etc.) and any add-on that exposes the
standard `sim/cockpit/radios/*` DataRefs. For exotic aircraft that don't
expose `com_power`, set `skip_radio_power_check: true` in `settings.json`.
Laminar's default ATC can be suppressed via `disable_default_atc`.

**Can I fly hands-on-yoke without focusing the plugin window?**
Yes — that's the design. Bind Push-To-Talk once to a yoke button or keyboard
key (X-Plane command `xp_wellys_atc/ptt`). After that, every interaction is
voice: press PTT, speak, release, hear ATC reply. The plugin window does not
need keyboard focus during flight, and all inference runs on background
threads so X-Plane never stutters.

**Does the plugin read my COM1/COM2 frequencies automatically?**
Yes. Active and standby frequencies for both COM radios are read live from
X-Plane DataRefs. The plugin also detects which radio is active and
auto-classifies the frequency type (ATIS / Ground / Tower / Approach /
UNICOM) against the apt.dat frequency database. No manual frequency entry.

**Does the plugin set the transponder / squawk code?**
No — spoken only. ATC may say "squawk 1200" (US flow), but the plugin does
not read or write the cockpit transponder DataRefs. You dial the squawk
manually on your transponder.

**How does it compare to BeyondATC or SayIntentions?**
Strengths: 100 % offline on Apple Silicon (no subscription, no cloud, no
constant internet required), ~1.16 s warm pipeline latency, ICAO-correct EU
phraseology with realistic Tower reactions to pilot errors.
Limitations today: VFR-only, no IFR, no routing, no traffic *sequencing*
(only traffic *awareness*), no transponder data link, no co-pilot. It is
not yet an all-in-one replacement for those products.

**Is there an introduction video?**
Not yet.

**How does it compare to OpenSquawk?**
Not yet evaluated.

## Project Structure

```
src/
├── main.cpp                # XPlugin* entry points, menu, flight loop
├── atc/                    # Session coordinator, state machine, intent
│                           #   parser, templates, ATIS, flight phase,
│                           #   engine, traffic_advisor, traffic_dialog
├── audio/                  # Push-to-talk, mic capture, PCM playback
│                           #   on the X-Plane radio bus (COM1 or COM2),
│                           #   mic permission
├── backends/               # Strategy interfaces + concrete WhisperStt /
│                           #   LlamaLm / PiperTts + manager (async
│                           #   dispatch) + loader (verify + load) +
│                           #   downloader (libcurl + resume + SHA256)
├── core/                   # Logging, XPlaneContext (SDK-free struct +
│                           #   SDK-coupled DataRef reader)
├── data/                   # Airport VRPs, apt.dat-derived airspace
│                           #   index, traffic_context (struct + 2 Hz
│                           #   TCAS reader), traffic_geometry helpers
├── persistence/            # settings.json, model_paths, model_manifest
└── ui/                     # Dear ImGui ATC panel + Models + Traffic tabs
```

The `xp_atc_engine` CMake OBJECT library compiles the SDK-free translation
units (`atc/`, `core/logging`, `core/xplane_context` struct, `data/`,
`backends/manager.cpp`, `persistence/model_manifest`). Both the plugin
module and the headless `atc_repl` tool reuse it. The plugin module adds
the SDK-coupled units (`main.cpp`, `audio/`, `core/xplane_context_runtime.cpp`,
`backends/{whisper_stt,llama_lm,piper_tts,loader,downloader}.cpp`,
`persistence/{settings,model_paths}.cpp`, `ui/atc_ui.cpp`) plus statically
linked `whisper`, `llama`, `common`, and a shared `libpiper.dylib` that
links to `libonnxruntime.1.22.0.dylib` — both dylibs co-located inside the
plugin bundle alongside the `.xpl` and resolved through `@loader_path`
rpath.

## Third-Party Dependencies

See [`THIRD_PARTY.md`](THIRD_PARTY.md) for the full list of bundled or
linked libraries, their licenses, and how they are vendored.

## Development Workflow

### CI Pipeline

The GitHub Actions pipeline runs in two situations only:

- **Pull Request against `main`** — validates the change (build + scenario tests) before it can be merged
- **Push of a version tag `v*`** — builds the release artifact and publishes a GitHub Release with the packaged ZIP

Direct pushes to `main` no longer trigger a build. All code changes must go
through a Pull Request.

### Merging to `main`

Branch protection requires:

1. PR (no direct pushes)
2. Status check `build-macos` passing (`make all` succeeds)
3. PR branch up to date with main

## License

This project is licensed under the [GNU General Public License v3.0](https://www.gnu.org/licenses/gpl-3.0.html).
GPLv3 is required because espeak-ng (GPL-3.0-or-later) is statically
linked into the bundled `libpiper.dylib`. Compatible with all other
bundled third-party libraries; see [`THIRD_PARTY.md`](THIRD_PARTY.md)
for the per-dependency breakdown.
