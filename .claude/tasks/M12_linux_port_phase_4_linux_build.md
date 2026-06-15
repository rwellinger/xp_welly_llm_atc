# M12 — Linux Port — Phase 4: Linux Build + Minimal Stubs

## Context

Cross-platform port on branch `feat/linux-port`. With Phases 0–3 done, the codebase is ready for a Linux compile target: an audit-safety-netted, abstracted, platform-split tree. This phase adds the Linux build toolchain and the smallest possible set of Linux source files needed for `xp_wellys_atc.xpl` to load on Linux X-Plane and reach the Settings panel.

**Scope discipline:** Linux is **cloud-only** in this phase. No whisper.cpp / llama.cpp / Piper. Local-Mode is forced off. OpenAI + Mistral are the only available backends. Keychain is **file-only** for now (libsecret upgrade lands in Phase 5). Clipboard is a stub returning `""` (xclip support lands in Phase 6). Mic-permission is a no-op (Linux has no system-level mic permission gate today).

The Linux fork at `/Users/robertw/Workspace/x-plane/linux/` is a useful **reference** for SDK paths and CMake snippets but should not be copy-pasted — it carries a broken Mistral implementation that violates the Backend Adapter Rule (Phase 0 will catch it if it sneaks in).

## Goal

`cmake -S . -B build && cmake --build build` succeeds on Linux x64. Produces `build/xp_wellys_atc.xpl` as an ELF shared object. macOS build remains unchanged.

## Steps

1. **CMakeLists top-level platform branching:**
   - Detect `CMAKE_SYSTEM_NAME STREQUAL "Linux"`
   - On Linux: `set(XPWELLYS_USE_LOCAL_INFERENCE OFF CACHE BOOL "..." FORCE)` — local backends excluded
   - On Linux: `set(XPWELLYS_AUDIO_INPUT_BACKEND "portaudio")` — default to PortAudio
   - X-Plane SDK Linux path: link against `sdk/Libraries/Lin/XPLM_64.so` (and XPWidgets where applicable)
   - Output target: still named `xp_wellys_atc.xpl` but produced as a Linux `.so` (CMake handles the suffix; X-Plane on Linux loads `.xpl` regardless of ELF magic)
2. **Linux dependencies via pkg-config / find_package:**
   - `find_package(OpenSSL REQUIRED)` — replaces CommonCrypto for SHA256
   - `find_package(CURL REQUIRED)` — already used; on Linux it's the system libcurl
   - `pkg_check_modules(PORTAUDIO REQUIRED IMPORTED_TARGET portaudio-2.0)` — system or vendored
3. **New Linux source files (minimal stubs):**
   - `src/audio/mic_permission_linux.cpp`:
     ```cpp
     #include "audio/mic_permission.hpp"
     namespace mic_permission { bool request() { return true; } }
     ```
   - `src/ui/clipboard_linux.cpp`:
     ```cpp
     #include "ui/clipboard.hpp"
     namespace clipboard { std::string read_text() { return ""; } }
     ```
     (real xclip/wl-paste implementation in Phase 6)
   - `src/persistence/keychain_linux.cpp`:
     - Implement `save / load / erase` against `~/.config/xp_wellys_atc/credentials.json`, file mode `0600`
     - Refuse to read if `stat()` shows world-readable
     - Implement `storage_backend()` returning `FILE_FALLBACK`
     - JSON keyed by `service + "/" + account`
     - Schema field `"warning"` set to `"owner-readable plaintext fallback — no system keyring detected"`
   - `src/persistence/sha256_openssl.cpp`:
     - Implement `sha256::sha256_hex(...)` against OpenSSL EVP (`EVP_MD_CTX_new` + `EVP_DigestInit_ex(EVP_sha256())` + `EVP_DigestUpdate` + `EVP_DigestFinal_ex`)
     - This snippet is small (~30 lines) and can be drafted from the OpenSSL docs; the Linux fork at `/Users/robertw/Workspace/x-plane/linux/src/persistence/model_manifest.cpp` is a reasonable reference
4. **CMake gating:**
   - `if(APPLE)` … `elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")` … blocks for: `mic_permission_*`, `clipboard_*`, `keychain_*`, `sha256_*`, audio input backend selection
5. **Build artifact staging:**
   - On Linux, `libportaudio.so` must be findable at runtime — either rely on the system package being installed (Ubuntu: `apt install libportaudio2`) or stage the `.so` next to `xp_wellys_atc.xpl` with `RPATH=$ORIGIN`. Pick system-package-required for simplicity in this phase; document in README. The Linux beta tester is expected to install `libportaudio2`, `libsecret-1-0`, `libcurl4`, `libssl3` via their package manager.
6. **`backends/loader.cpp` review:**
   - Verify the existing `local → openai` rewrite logic for the x86_64 path also fires on Linux when `XPWELLYS_USE_LOCAL_INFERENCE=OFF`
   - Mistral selection must be honored on Linux (it is honored on x86_64 Mac per CLAUDE.md)
7. **Smoke test plan for the Linux beta tester:**
   - Load `.xpl` in X-Plane Linux, verify menu entry appears
   - Open Settings tab, paste OpenAI key, press Save Key
   - Open ATC panel, PTT a short phrase, expect a TTS reply on the radio bus
   - Repeat with Mistral key
   - Send `Log.txt` back for review

## Files Affected

- **New:** `src/audio/mic_permission_linux.cpp`, `src/ui/clipboard_linux.cpp`, `src/persistence/keychain_linux.cpp`, `src/persistence/sha256_openssl.cpp`
- **Modified:** `CMakeLists.txt` (substantial), `README.md` (Linux dependencies + experimental note — minimal blurb here, deeper docs in Phase 7)

## CMake Changes

Major. New Linux branch in the top-level file. `XPWELLYS_USE_LOCAL_INFERENCE` forced off on Linux. OpenSSL + PortAudio dependencies wired. Per-file `if(APPLE)/elseif(LINUX)` selection for all four platform modules.

## Commit Message

```
build: add Linux target (cloud-only, PortAudio, file keychain)

CMake gains a Linux branch:
- XPWELLYS_USE_LOCAL_INFERENCE forced OFF (no whisper/llama/piper)
- PortAudio input via pkg-config (system libportaudio2)
- OpenSSL EVP for SHA256
- libsecret deferred to Phase 5; keychain is file-only here
- xclip deferred to Phase 6; clipboard is stub
- mic-permission is no-op

Linux build produces xp_wellys_atc.xpl ELF .so. macOS build unaffected.
Requires beta-tester install: libportaudio2, libcurl4, libssl3.

Closes M12 Phase 4.
```

## Validation

- macOS: `make all` still green, no behavior change
- Linux (beta tester or Docker/Ubuntu CI image): `cmake --build` succeeds, `.xpl` produced
- Beta tester loads plugin in X-Plane Linux, runs through OpenAI + Mistral smoke tests
- `Log.txt` from tester shows `BACKEND MODE: openai` / `BACKEND MODE: mistral` audit banners

## Depends On

- Phases 0–3 (all)

## Estimated Effort

~1 day for the maintainer + 2–5 days calendar time for tester feedback loops
