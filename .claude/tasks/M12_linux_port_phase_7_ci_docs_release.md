# M12 — Linux Port — Phase 7: CI, Docs, Experimental Release

## Context

Cross-platform port on branch `feat/linux-port`. Phases 0–6 produced a working Linux build with cloud backends, libsecret-or-file keychain, and real clipboard support. Now we close the loop: add a Linux build job to CI so future regressions are caught at PR time, document the experimental status honestly, and prepare a tagged release the beta tester can install without checking out source.

**No runtime tests on Linux in CI** — GitHub Actions runners have no microphone and no X-Plane install. The Linux CI job is a **compile-and-link gate only**, but that already filters 80% of cross-platform regressions (missing headers, wrong library names, ODR violations between TUs).

## Goal

GitHub Actions runs the Linux build on every PR. README clearly markets Linux as experimental beta. THIRD_PARTY.md credits the new dependencies. Tagged release ships the macOS Universal Binary + a Linux x64 archive side by side.

## Steps

1. **GitHub Actions matrix:**
   - Add `ubuntu-22.04` job alongside the existing `macos-14` job
   - Linux job steps:
     - `apt install -y cmake clang-15 libcurl4-openssl-dev libssl-dev portaudio19-dev libsecret-1-dev pkg-config`
     - `git submodule update --init --recursive` for vendor deps
     - `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release`
     - `cmake --build build --parallel`
     - Upload `build/xp_wellys_atc.xpl` as artifact named `xp_wellys_atc-linux-x64.xpl`
   - macOS job: unchanged
2. **Release workflow:**
   - When a tag matching `v*` is pushed, package:
     - `xp_wellys_atc-macos-universal.zip` (existing)
     - `xp_wellys_atc-linux-x64.zip` containing the Linux `.xpl` + a small `INSTALL.md` listing the apt packages
3. **README.md updates:**
   - New section near the top: `Platform Support`
     - macOS arm64: fully supported, all three backends
     - macOS x86_64: supported, cloud backends only (OpenAI + Mistral)
     - Linux x64: **experimental, beta testers wanted** — cloud backends only, beta tester required for any bug fixes since maintainer has no Linux hardware
   - Linux install section: list required apt packages (`libportaudio2`, `libsecret-1-0`, `libcurl4`, `libssl3`)
   - Note that Linux uses libsecret if available, otherwise falls back to a plaintext credentials file
4. **THIRD_PARTY.md:**
   - Add PortAudio (MIT-style license — verify before merge)
   - Add OpenSSL (Apache 2.0)
   - Add libsecret (LGPL 2.1) — verify GPL-3.0 compatibility (should be fine, LGPL is GPL-compatible)
5. **VERSION.txt + RELEASE.md:**
   - Bump version (next minor — e.g. `0.X.0`)
   - RELEASE.md entry: `Experimental Linux x64 support (cloud backends only). Beta testers welcome.`
6. **CLAUDE.md updates:**
   - New section `Platform Layout` describing the per-OS file splits
   - Update the Build System section: mention `XPWELLYS_AUDIO_INPUT_BACKEND` option
   - Document the cloud-only constraint on Linux explicitly

## Files Affected

- **New / modified:** `.github/workflows/*.yml`
- **Modified:** `README.md`, `THIRD_PARTY.md`, `RELEASE.md`, `VERSION.txt`, `CLAUDE.md`

## CMake Changes

None directly — CI invokes existing build.

## Commit Message

```
ci+docs: experimental Linux x64 support

GitHub Actions matrix gains an ubuntu-22.04 build-only job. Release
workflow now packages a Linux x64 zip alongside the macOS Universal.
README marks Linux as experimental, lists apt dependencies, documents
the libsecret-or-file keychain behavior. CLAUDE.md updated with the
platform-split layout from M12 Phases 3-6.

Closes M12 Phase 7. Closes M12.
```

## Validation

- GitHub Actions: macOS job stays green, Linux job goes green on first run
- A clean Ubuntu 22.04 VM (or Docker container) can `apt install` the listed packages, download the release zip, drop the `.xpl` into `X-Plane 12/Resources/plugins/xp_wellys_atc/64/`, and load X-Plane successfully
- Beta tester confirms install instructions work as written

## Depends On

- Phases 0–6

## Estimated Effort

~½ day code + iteration with the beta tester until install docs are verified
