# xp_wellys_atc — Linux Port

Linux (Ubuntu 24.04 / Zorin OS 18.1) support for [xp_wellys_atc](README.md).

> **Status**: working, tested on Zorin OS 18.1 + X-Plane 12.  
> Branch: `feat/linux-port-pulseaudio` — PR #9 against upstream.

---

## Supported platforms

| OS | Arch | Backends available |
|---|---|---|
| Ubuntu 24.04 LTS | x86_64 | **Local** (whisper.cpp + llama.cpp + Piper, CPU-only), **OpenAI Cloud**, **Mistral Cloud** |
| Zorin OS 18.1 | x86_64 | same |
| Other systemd-based distros with PipeWire/PulseAudio | x86_64 | likely works, untested |

> No GPU required — all local inference runs on CPU.  
> Llama 3.2 3B Q4 takes ~5–15 s on a modern CPU; Piper TTS is always fast.

---

## Runtime requirements

| Item | Requirement |
|---|---|
| X-Plane | 12 (tested 12.1+) |
| Audio | PulseAudio or PipeWire with PulseAudio compatibility layer (default on Ubuntu 22.04+) |
| Clipboard paste | `wl-clipboard` (Wayland) or `xclip` (X11) — optional, only needed for the \[Paste\] button in Settings |
| Disk | ~2.1 GB for local models (downloaded on first launch) + ~19 MB espeak-ng-data (bundled) |

---

## Install (pre-built binary)

1. Download the latest archive:
   ```
   xp_wellys_atc_v<version>-linux-port-<commit>.tar.gz
   ```

2. Extract into your X-Plane plugins directory:
   ```bash
   tar -xzf xp_wellys_atc_v*-linux-port-*.tar.gz \
       -C ~/X-Plane\ 12/Resources/plugins/
   ```

3. Plugin directory layout after extraction:
   ```
   X-Plane 12/Resources/plugins/xp_wellys_atc/
   ├── lin_x64/
   │   ├── xp_wellys_atc.xpl
   │   ├── libpiper.so
   │   ├── libonnxruntime.so.1        -> libonnxruntime.so.1.22.0
   │   ├── libonnxruntime.so.1.22.0
   │   ├── libonnxruntime_providers_shared.so
   ├── Resources/
   │   ├── espeak-ng-data/            (bundled, ~19 MB)
   │   └── models/                    (empty — filled on first launch)
   └── data/
       ├── settings.json
       ├── models_catalog.json
       ├── atc_prompt_templates.json
       ├── vrps/airport_vrps.json
       └── atc_profiles/eu/ us/ de/
   ```

4. Launch X-Plane. On first load, open the **ATC** menu → **Models** tab and download the three local models (~2.1 GB total). Or switch to OpenAI/Mistral Cloud in Settings to skip the download.

5. Optional — install clipboard tool for the \[Paste\] button:
   ```bash
   # Wayland (default on Ubuntu 22.04+ / Zorin 18)
   sudo apt install wl-clipboard

   # X11
   sudo apt install xclip
   ```

---

## Build from source

### Dependencies

```bash
sudo apt install \
    build-essential cmake ninja-build ccache git \
    libpulse-dev \
    libssl-dev \
    libcurl4-openssl-dev \
    libgl-dev \
    pkg-config \
    patchelf
```

> `patchelf` is required — it rewrites `libpiper.so`'s RPATH to `$ORIGIN`
> as a post-build step so the plugin loads on machines other than the build host.

### Build

```bash
git clone --recurse-submodules <repo-url>
cd xp_welly_llm_atc

make setup    # downloads SDK, ImGui, nlohmann/json, Catch2
make build    # CMake Release -> build-pr/xp_wellys_atc.xpl
make install  # copies to ~/X-Plane 12/Resources/plugins/xp_wellys_atc/
```

Override the X-Plane path if needed:
```bash
make install XPLANE_ROOT=/path/to/xplane
```

### Tests

```bash
make test       # Catch2 unit + scenario tests
make sanitize   # ASan + UBSan on SDK-free engine code
make repl       # headless CLI — no X-Plane, no audio, no models
```

---

## API key storage (Linux)

On macOS the plugin uses the system Keychain. On Linux, keys are stored as
permission-restricted files:

```
~/.config/xp_wellys_atc/
├── com.xp_wellys_atc.openai_default.key    (chmod 0600)
└── com.xp_wellys_atc.mistral_default.key   (chmod 0600)
```

Keys are never written to `settings.json` or `Log.txt` (only the last 4
characters appear in logs for audit purposes).

---

## Known cosmetic issues

| Issue | Cause | Impact |
|---|---|---|
| Resize cursor appears as a down-arrow | ImGui X11 cursor mapping on XWayland/GNOME | Cosmetic only |
| UI font is basic bitmap | ImGui default embedded font (same on macOS) | Cosmetic only |

Neither issue affects functionality.

---

## Shared libraries — technical note

All shared libraries (`libpiper.so`, `libonnxruntime.so.1`) are resolved via
`$ORIGIN` RPATH — they must live in the same `lin_x64/` directory as
`xp_wellys_atc.xpl`. Do not move the `.xpl` without the accompanying `.so`
files.

`XPLM_64.so` is provided by X-Plane itself and does not need to be bundled.
`libcurl`, `libpulse`, `libstdc++`, `libgomp` are expected to be present on
the system (`libcurl4`, `libpulse0`, `libstdc++6`, `libgomp1` on Ubuntu).
