# Welly's ATC — AI Voice ATC for X-Plane 12

![Welly's ATC panel with ATIS broadcast at LSZB Bern-Belp](images/atc-atis-example.jpg)

> **Dual-backend X-Plane 12 ATC plugin: local inference or OpenAI Cloud, your choice.**
>
> - **Local (Apple Silicon, default)** — `whisper.cpp` (Metal) + `llama.cpp`
>   (Metal) + Piper TTS, fully offline once the models are downloaded.
>   No daemons, no helper apps, no API keys.
> - **OpenAI Cloud (any Mac)** — Whisper API + Chat Completions +
>   TTS API. Bring your own API key (stored in the macOS Keychain).
>   The only option on Intel Macs.
>
> The plugin ships as a **universal binary**: the arm64 slice carries
> both backends, the x86_64 slice is OpenAI-only. The user picks the
> mode at runtime in Settings.
>
> The spike-phase architecture and per-backend measurements are archived in
> [`docs/architecture-analysis.md`](docs/architecture-analysis.md) and
> [`spikes/spike_e2e/RESULTS.md`](spikes/spike_e2e/RESULTS.md).
>
> **Measured pipeline latency** (warm, M4, local inference, end-to-end spike):
> STT 321 ms · LM 634 ms · TTS 200 ms · **total ≈ 1.16 s per request** —
> well under the 3 s acceptance target with > 1.8 s of headroom for the
> M4-vs-M1 generational gap and the plugin's main-thread / Core Audio
> overhead. OpenAI Cloud is typically slower: 2–3 s warm round-trip
> dominated by API latency. M1 local re-validation: pending real-flight
> smoke test.

---

AI-powered ATC voice communication plugin for X-Plane 12 VFR flights.

Talk to ATC using your microphone via push-to-talk. The plugin
transcribes your speech (locally with whisper.cpp **or** via the
OpenAI Whisper API, your pick), interprets your intent through a
rule-based ATC state machine — with a low-confidence fallback to a
local Llama 3.2 3B classifier or OpenAI's `gpt-4o-mini`, again your
pick — and plays back ATC responses synthesised locally with Piper or
via the OpenAI TTS API.

## Features

- **Push-to-Talk** — bound via X-Plane command binding (keyboard or joystick)
- **Dual-backend inference** — pick **Local** (Apple Silicon only) or
  **OpenAI Cloud** (any Mac, BYO API key) in the Settings tab. Switch
  at runtime, no plugin restart. Every inference call is tagged with
  `[STT-LOCAL]` / `[STT-OPENAI]` (and equivalent for LM/TTS) in
  X-Plane's `Log.txt` so you can audit which side served each request.
- **Local Speech-to-Text** — `whisper.cpp` `small.en-q5_1`, Metal-accelerated
- **Local LLM** — `llama.cpp` running Llama 3.2 3B Instruct (Q4_K_M),
  Metal-accelerated; used for intent disambiguation when the rule-based
  parser is uncertain. Repair output is digit-validated to suppress
  hallucinated runways or frequencies.
- **Local Text-to-Speech** — Piper, neutral US accent (`en_US-lessac-medium`),
  CPU + onnxruntime
- **OpenAI Cloud option** — `whisper-1` for STT, `gpt-4o-mini` for the
  intent classifier (JSON-mode for constrained output), `tts-1` with
  six selectable voices (`alloy/echo/fable/onyx/nova/shimmer`; `onyx`
  is closest to real ATC). Key stored in the macOS Keychain via
  `Security.framework`, never in `settings.json`, never logged in full
  (only the last 4 characters appear in audit lines).
- **ATC State Machine** — VFR phraseology for towered and non-towered airports
- **Flight Phase Detection** — context-aware guards prevent unrealistic ATC
  interactions based on aircraft state (parked, taxi, airborne, etc.)
- **Live Traffic Awareness (v2.1) + Landing Sequencing (v2.2)** —
  provider-agnostic `sim/cockpit2/tcas/targets/...` reader feeding a
  2 Hz `TrafficContext` snapshot. EU-phraseology en-route advisories
  with voice acknowledgement (`"Traffic in sight"` / `"Negative
  contact"` / `"Looking"`) on a side-channel that does not interfere
  with the main ATC flow. **v2.2 adds VFR landing sequencing** —
  *"number N, follow the [type] on left base, cleared to land runway X"*
  when other traffic is on Final or Pattern, *"continue approach,
  traffic on the runway"* when the active runway is blocked, and an
  unsolicited Tower-issued go-around within 1 NM of the threshold when
  the runway stays occupied. Master switch `traffic_features_enabled`
  in Settings turns the whole subsystem off in one click.
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

The plugin ships as a **universal binary** — one `.xpl`, two slices.
X-Plane automatically loads whichever matches the host.

| Mac | Slice loaded | Backends available |
|---|---|---|
| Apple Silicon (M1 / M2 / M3 / M4) | `arm64` | **Local** *or* **OpenAI Cloud** |
| Intel (x86_64) | `x86_64` | **OpenAI Cloud only** (local inference needs Metal + Apple Silicon) |

| Resource | Local mode | OpenAI Cloud mode |
|---|---|---|
| RAM | 32 GB recommended (X-Plane 12 + ~3 GB headroom for the inference stack) | 16 GB (no model in RAM — calls are stateless HTTP requests) |
| Disk | ~2.5 GB free for the bundled models | ~50 MB for the plugin bundle (no models downloaded) |
| GPU | Any Metal-capable GPU on the same Apple Silicon chip | not used |
| Network | not used at runtime (one-time model download from HuggingFace) | required — every PTT release triggers HTTPS calls to `api.openai.com` |

OpenAI Cloud mode also costs money per request (Whisper API + Chat
Completions + TTS API). Latency is typically 2–3 s warm vs. 1–1.5 s
warm for local inference. Plan accordingly.

## Software Requirements

| Item | Requirement |
|---|---|
| macOS | **13.3 or later** (onnxruntime 1.22.0 requires this on the arm64 slice; the x86_64 slice inherits the same deployment target so the lipo'd binary stays consistent) |
| X-Plane | X-Plane 12 (12.0 or later) |
| OpenAI account | Only if you want to use OpenAI Cloud mode — needs an API key with billing enabled. Local mode has no cloud dependency. |
| For building from source | CMake 3.26+, Homebrew LLVM (`brew install llvm`), Xcode Command Line Tools |

## Quick Start (pre-built release)

1. Download `xp_wellys_atc-vX.Y.Z.zip` from the GitHub Releases page.
   The `.xpl` inside is a universal binary covering arm64 and x86_64.
2. Extract into `X-Plane 12/Resources/plugins/`. Result:
   ```
   X-Plane 12/Resources/plugins/xp_wellys_atc/
     ├── mac_x64/
     │     ├── xp_wellys_atc.xpl       (universal: arm64 + x86_64)
     │     ├── libpiper.dylib          (used by arm64 slice only)
     │     ├── libonnxruntime.1.22.0.dylib
     │     └── libonnxruntime.dylib
     ├── Resources/
     │     └── espeak-ng-data/   (~19 MB, used by arm64 slice only)
     └── data/
           └── (templates, region rules, etc.)
   ```
3. Launch X-Plane. Open the plugin window via *Plugins → Welly's ATC*.
4. **Pick your backend** in the **Settings** tab:
   - **Local** (Apple Silicon, default): the **Models** tab shows three
     rows in red. Click **Download all missing** — the plugin pulls
     ~2.0 GB from HuggingFace over HTTPS. Resumable; cancellable;
     SHA256-verified after each file. Once all rows show **Ready**
     (green), the PTT-disabled banner on the Status tab disappears.
   - **OpenAI Cloud** (any Mac): paste your OpenAI API key into the
     **OpenAI API Key** field in Settings (use the `[Paste]` button —
     Cmd+V inside X-Plane's ImGui context is unreliable). Click
     **Save Key**. The key is stored in the macOS Keychain under
     service `com.xp_wellys_atc.openai`. PTT is enabled immediately;
     no model download.
5. Fly. The Status tab's banner will tell you which mode is active and
   `Log.txt` carries a one-line `BACKEND MODE: ...` banner on every
   load so you can prove after the fact which side served the session.

## Backend Modes

You can switch at any time in the Settings tab — the plugin tears
down the active inference stack and brings up the other one, no
X-Plane restart. Source-level invariant: the three local backends
(`whisper_stt.cpp`, `llama_lm.cpp`, `piper_tts.cpp`) contain no
`#include` of the OpenAI clients and zero `curl_easy_perform` calls;
the three OpenAI clients (`openai_stt.cpp`, `openai_lm.cpp`,
`openai_tts.cpp`) contain no `#include` of `whisper.h` / `llama.h` /
`piper.h`. So in Local mode no code path can call OpenAI, and vice
versa — verified by `tests/test_audit_logging.cpp`.

Auditing which mode served a request: grep `Log.txt`.

| Tag in `Log.txt` | What it means |
|---|---|
| `[xp_wellys_atc] BACKEND MODE: LOCAL ...` | Loader brought up the local pipeline. |
| `[xp_wellys_atc] BACKEND MODE: OPENAI (api.openai.com) ...` | Loader brought up the cloud pipeline. |
| `[STT-LOCAL] / [LM-LOCAL] / [TTS-LOCAL]` | Per-call audit for each local inference. |
| `[STT-OPENAI] / [LM-OPENAI] / [TTS-OPENAI]` | Per-call audit for each cloud inference. API key is truncated to its last 4 chars (`sk-...ABCD`). |

## Build From Source

```sh
git clone --recurse-submodules <repo-url>
cd xp_welly_llm_atc
make setup     # X-Plane SDK, Dear ImGui, nlohmann/json, Catch2
make build     # Universal Release build → build/xp_wellys_atc.xpl (arm64
               # with both backends + x86_64 cloud-only, lipo'd into one
               # .xpl). This is now the only build target — there is no
               # arm64-only fast-path anymore.
make install   # Code-sign + install to X-Plane plugins directory
```

`make build` runs CMake twice (arm64 with `XPWELLYS_USE_LOCAL_INFERENCE=ON`
in `build-arm64/`, x86_64 with the same flag `OFF` in `build-x86_64/`)
and `lipo`-merges the two `.xpl`s into one universal binary. Build
time is roughly double a single-arch build; that is the deliberate
trade-off so dev and release artifacts are byte-for-byte identical in
shape. For tag-driven release builds pass `RELEASE_FLAG=-DRELEASE=ON`
(`make release-build` does this for you — embeds the version from
`VERSION.txt`).

The build downloads onnxruntime's prebuilt arm64 dylib (≈ 33 MB) into
`spikes/spike_piper/third_party/piper1-gpl/libpiper/lib/` on first
configure. After that everything is local. The x86_64 slice has no
onnxruntime / Piper / whisper / llama dependency at all — it links
only against libcurl + the system frameworks (Security, AudioToolbox,
etc.) and the OpenAI clients.

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

| Model | Lang | Size | SHA256 | URL |
|---|---|---:|---|---|
| `ggml-small.en-q5_1.bin` | en | 181 MB | `bfdff4894dcb76bbf647d56263ea2a96645423f1669176f4844a1bf8e478ad30` | [`huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.en-q5_1.bin`](https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small.en-q5_1.bin) |
| `ggml-small-q5_1.bin` | de (multilingual) | 181 MB | `ae85e4a935d7a567bd102fe55afc16bb595bdb618e11b2fc7591bc08120411bb` | [`huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small-q5_1.bin`](https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small-q5_1.bin) |
| `Llama-3.2-3B-Instruct-Q4_K_M.gguf` | — | 1.88 GB | `6c1a2b41161032677be168d354123594c0e6e67d2b9227c84f296ad037c728ff` | [`huggingface.co/bartowski/Llama-3.2-3B-Instruct-GGUF/resolve/main/Llama-3.2-3B-Instruct-Q4_K_M.gguf`](https://huggingface.co/bartowski/Llama-3.2-3B-Instruct-GGUF/resolve/main/Llama-3.2-3B-Instruct-Q4_K_M.gguf) |
| `en_US-lessac-medium.onnx` | en | 60 MB | `5efe09e69902187827af646e1a6e9d269dee769f9877d17b16b1b46eeaaf019f` | [`huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/lessac/medium/en_US-lessac-medium.onnx`](https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/lessac/medium/en_US-lessac-medium.onnx) |
| `en_US-lessac-medium.onnx.json` | en | 4.9 KB | `efe19c417bed055f2d69908248c6ba650fa135bc868b0e6abb3da181dab690a0` | [`huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/lessac/medium/en_US-lessac-medium.onnx.json`](https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/lessac/medium/en_US-lessac-medium.onnx.json) |
| `de_DE-thorsten-medium.onnx` | de | 60 MB | `7e64762d8e5118bb578f2eea6207e1a35a8e0c30595010b666f983fc87bb7819` | [`huggingface.co/rhasspy/piper-voices/resolve/main/de/de_DE/thorsten/medium/de_DE-thorsten-medium.onnx`](https://huggingface.co/rhasspy/piper-voices/resolve/main/de/de_DE/thorsten/medium/de_DE-thorsten-medium.onnx) |
| `de_DE-thorsten-medium.onnx.json` | de | 4.7 KB | `974adee790533adb273a1ac88f49027d2a1b8f0f2cf4905954a4791e79264e85` | [`huggingface.co/rhasspy/piper-voices/resolve/main/de/de_DE/thorsten/medium/de_DE-thorsten-medium.onnx.json`](https://huggingface.co/rhasspy/piper-voices/resolve/main/de/de_DE/thorsten/medium/de_DE-thorsten-medium.onnx.json) |

Lang column: `en` files are required for the EU/US regions, `de` files
for the DE region. Llama is multilingual and shared. The Models tab
filters rows by `settings::backend_language()` by default and exposes
a **Show all languages** toggle for power users who want to keep both
sets on disk.

After dropping the files in, reopen the plugin window — the Models tab
runs SHA256 verification in the background and flips the rows to **Ready**
once each hash matches.

### M6 SHA256 verification procedure (DE models)

The three DE-row hashes above were captured on 2026-06-04 against
HuggingFace `main`. To re-verify (or repin after an upstream model
update) run:

```bash
# Whisper small multilingual (~184 MB)
curl -L -o /tmp/ggml-small-q5_1.bin \
  https://huggingface.co/ggerganov/whisper.cpp/resolve/main/ggml-small-q5_1.bin
shasum -a 256 /tmp/ggml-small-q5_1.bin
stat -f%z /tmp/ggml-small-q5_1.bin

# Piper de_DE-thorsten-medium (.onnx ~63 MB, .onnx.json ~5 KB)
curl -L -o /tmp/de_DE-thorsten-medium.onnx \
  https://huggingface.co/rhasspy/piper-voices/resolve/main/de/de_DE/thorsten/medium/de_DE-thorsten-medium.onnx
curl -L -o /tmp/de_DE-thorsten-medium.onnx.json \
  https://huggingface.co/rhasspy/piper-voices/resolve/main/de/de_DE/thorsten/medium/de_DE-thorsten-medium.onnx.json
shasum -a 256 /tmp/de_DE-thorsten-medium.onnx /tmp/de_DE-thorsten-medium.onnx.json
stat -f%z /tmp/de_DE-thorsten-medium.onnx /tmp/de_DE-thorsten-medium.onnx.json
```

Paste the three SHA256 hashes + three sizes into:
- `src/persistence/model_manifest.cpp` `voice_catalog()` (Thorsten row:
  two hashes + two sizes)
- `src/persistence/model_manifest.cpp` `manifest()` (multilingual Whisper:
  one hash + one size)
- The table above (three rows)

### Expected first-run download time

5–30 minutes on typical home internet; the bottleneck is HuggingFace's
download throughput, not the plugin. The downloader resumes via HTTP
`Range` if your link drops, so a Wi-Fi blip mid-Llama does not restart
the 1.88 GB pull from scratch.

## Configuration

Settings live in `<plugin>/data/settings.json`. The OpenAI API key
is the only secret — and it lives in the macOS Keychain, never in
this file.

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
| `traffic_features_enabled` | `true` | Master switch for the traffic subsystem (advisories, landing sequencing, go-around trigger). Off → `traffic_context::update()` returns an empty snapshot and every downstream consumer becomes a no-op. Requires a traffic provider (LiveTraffic, xPilot, swift, X-IvAp, native AI) to do anything anyway. |
| `backend_mode` | `local` | `local` (whisper + llama + Piper, arm64 only) or `openai` (Whisper API + Chat Completions + TTS API). The x86_64 slice silently rewrites this to `openai` at startup since Local is not available there. |
| `api_key_saved` | `false` | Flag only — set automatically when the user clicks **Save Key** in Settings. The actual key sits in the macOS Keychain under service `com.xp_wellys_atc.openai` / account `default`. Cleared by **Delete Key**. |
| `openai_stt_model` | `whisper-1` | OpenAI Whisper model ID for the STT call. |
| `openai_lm_model` | `gpt-4o-mini` | OpenAI Chat Completions model ID for the intent classifier. JSON mode is enabled automatically. |
| `openai_tts_model` | `tts-1` | OpenAI TTS model ID. Set to `tts-1-hd` for higher-quality (slower) output. |
| `openai_tts_voice_atis` / `openai_tts_voice_tower` / `openai_tts_voice_ground` | `onyx` / `echo` / `alloy` | Per-role OpenAI voice. One of `alloy / echo / fable / onyx / nova / shimmer`. `onyx` is closest to real ATC. |

ATC response templates are defined in `data/regions/{eu,us}/atc_templates.json`.
Flight phase detection thresholds, ATC precondition guards, frequency guards,
and auto-correction rules are in `data/regions/{eu,us}/flight_rules.json`.
Switching the Region setting hot-reloads both files. All data files can be
edited without rebuilding the plugin.

### Airport Database (`data/regions/{eu,us,de}/airport_vrps.json`)

Per-airport configuration for Visual Reporting Points (VRPs) and traffic
pattern directions, scoped per region. Pre-populated for common Swiss
and German VFR airports. Each top-level key is an ICAO code with optional
fields:

- `name` — display name
- `pattern_direction` — per-runway `"left"` / `"right"` (overrides the
  global `pattern_direction` setting); accepts a string for an unconditional
  default or an object keyed by runway designator with optional `_default`
- `vrps` — array of `{ name, lat, lon, alt_ft }`; `name` is the phonetic
  spelling (e.g. `"November"`) so Whisper and Piper handle it cleanly
- `arrival_routes` — per-runway ordered list of VRP names used for
  inbound routing
- `_source` / `_comment` — optional audit annotations; ignored by the loader

#### Optional user override (Navigraph Charts workflow)

The bundled DE-region data only covers a handful of airports with verified
VRPs; the others ship with `pattern_direction` only (`vrps: []`) until they
are checked against an authoritative source. If you have a **Navigraph
Charts** subscription you can supply your own VRP coordinates without
forking the plugin:

1. Drop a JSON file under
   `<X-Plane>/Output/preferences/xp_wellys_atc/airport_vrps_<region>.json`
   (`<region>` is lowercase, e.g. `airport_vrps_de.json`). The directory
   is created on first plugin start. This path survives plugin re-installs.
2. Use the same schema as the bundled file. Per-ICAO entries fully replace
   the plugin defaults — there is no field-level merge, so include the
   complete entry for every airport you want to override.
3. Restart X-Plane (or `Reload Settings` from the menu) — a log banner in
   `Log.txt` confirms the load:
   `Airport VRPs loaded: N airports (X plugin, Y user overrides: Z replaced, W added) from <path>`

Navigraph Charts workflow per airport:
- Open the **VFR Approach Chart** (German charts: AD 2 EDxx, section
  *Visual Approach* or *VFR-Anflug*).
- Read the VRP code (W/N/E/S/Z…), translate to the phonetic name
  (`W` → `Whiskey`, `N` → `November`, …) — this is what Whisper
  transcribes and what Piper pronounces.
- Hover the chart for cursor lat/lon (Navigraph Charts displays the
  pointer coordinates in the toolbar).
- Read the published transit altitude from the chart legend.
- Note the pattern direction per runway from the AIP AD 2.22 (Flight
  Procedures) section.

The Navigraph **FMS Data** add-on for X-Plane Custom Data does *not*
contain VRPs (ARINC-424 is IFR-only). You need the Navigraph **Charts**
product to read the VFR data.

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
make all           # clean + format + build + lint + test (full local CI)
make build         # universal: arm64 (local+cloud) + x86_64 (cloud-only), lipo'd
make release-build # same as `make build` but passes -DRELEASE=ON (embeds VERSION.txt)
make test          # unit tests + scenario tests
make install       # code-sign + install to X-Plane
make repl          # build the headless atc_repl tool
make format        # clang-format
make lint          # clang-tidy (some rules promoted to errors)
make clean         # remove build/, build-arm64/, build-x86_64/, build-lint/, build-sanitize/
make distclean     # also remove sdk/, vendor/
```

## Known Limitations

| Limitation | Impact | Effort |
|---|---|---|
| **Local inference is Apple Silicon only** | Intel Macs can run the plugin via the x86_64 slice but only in OpenAI Cloud mode (requires API key + billing) | Resolved by the universal binary; lifting the Intel restriction for Local mode would need Metal alternatives + an x86_64 onnxruntime build |
| **English only** | non-English ATC not supported | Medium — would need different whisper / Llama / Piper voice |
| **OpenAI voices speak German with a US accent** | When `flow_region=DE` + `backend_mode=openai`, Whisper transcription and the LM respond in German correctly, but the tts-1 voices (`alloy`, `echo`, `fable`, `onyx`, `nova`, `shimmer`) are English-trained and render German with a noticeable US accent — NATO phonetic letters in particular sound anglophone (e.g. "Tshaar-lee" instead of "Tschar-li"). Acceptable for casual practice but unrealistic for BZF/AZF-style training. | Resolved by M6 (Local-mode Piper `de_DE-thorsten`) — until then, accept the accent or stick to English on the cloud pipeline |
| **Single-voice TTS** | All ATC speakers (Tower, Ground, ATIS) use the same Piper voice; ATIS speaks slower via `length_scale=1.18` | Low — could ship more voices and add a per-frequency selector |
| **"via Alpha" hardcoded** — taxiway name is always Alpha | Unrealistic at airports with different taxiway layouts | High — would need taxiway data from apt.dat or WED |
| **No wake-turbulence spacing** — sequencing in v2.2 picks number-by-distance only, no Light/Medium/Heavy separation | Acceptable for GA pattern work; missing for mixed-weight ops | Phase 5 on roadmap |
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
Strengths: 100 % offline option on Apple Silicon (no subscription, no
cloud, no constant internet required — at the user's discretion), ~1.16 s
warm pipeline latency in local mode, ICAO-correct EU phraseology with
realistic Tower reactions to pilot errors. An OpenAI Cloud mode is
available as a paid opt-in (BYO key) for users who prefer cloud LLMs
or run an Intel Mac.
Limitations today: VFR-only, no IFR, no routing, no wake-turbulence
spacing (sequencing in v2.2 is distance-only — Phase 5 on roadmap), no
transponder data link, no co-pilot. It is not yet an all-in-one
replacement for those products.

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
├── backends/               # Strategy interfaces + manager (async
│                           #   dispatch) + loader (verify + load) +
│                           #   downloader (libcurl + resume + SHA256).
│                           #   Concrete backends split by mode:
│                           #   Local: WhisperStt / LlamaLm / PiperTts
│                           #     (arm64 slice only, gated on
│                           #     XPWELLYS_USE_LOCAL_INFERENCE).
│                           #   Cloud: OpenAiStt / OpenAiLm / OpenAiTts
│                           #     (both slices). The two client sets
│                           #     share no headers and no code path —
│                           #     audit invariant enforced by tests.
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
`backends/{loader,downloader,openai_*}.cpp`,
`persistence/{settings,model_paths,keychain}.cpp`, `ui/atc_ui.cpp`).
The arm64 slice additionally compiles
`backends/{whisper_stt,llama_lm,piper_tts}.cpp` and links statically
against `whisper`, `llama`, `common`, plus a shared `libpiper.dylib`
that resolves `libonnxruntime.1.22.0.dylib` through `@loader_path` —
both dylibs co-located inside the plugin bundle alongside the `.xpl`.
The x86_64 slice has none of those dependencies; it links only
libcurl + Security + the audio frameworks.

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
