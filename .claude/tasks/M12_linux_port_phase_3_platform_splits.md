# M12 — Linux Port — Phase 3: Platform-Specific File Splits (macOS-only validation)

## Context

Cross-platform port on branch `feat/linux-port`. Four modules currently use macOS-only APIs in single TUs:

- `src/audio/mic_permission.mm` — AVFoundation
- `src/ui/clipboard.mm` — NSPasteboard
- `src/persistence/keychain.cpp` — `Security.framework`
- `src/persistence/model_manifest.cpp` — CommonCrypto for SHA256

To make room for Linux equivalents in Phase 4, we split each into per-platform TUs **while still only building on macOS**. The public headers stay byte-identical so no caller changes. This phase is purely a file reorganization with CMake gating — zero behavior change on macOS.

## Goal

Reorganize macOS-coupled code into platform-suffixed TUs with `if(APPLE)` CMake gates. After this phase, `main`-merge behavior on macOS is unchanged; the Linux build is ready to receive parallel implementations in Phase 4.

## Steps

1. **Mic permission split:**
   - Rename `src/audio/mic_permission.mm` → `src/audio/mic_permission_macos.mm`
   - Keep `src/audio/mic_permission.hpp` untouched (public API)
   - CMake: build `mic_permission_macos.mm` only when `APPLE`
2. **Clipboard split:**
   - Rename `src/ui/clipboard.mm` → `src/ui/clipboard_macos.mm`
   - Keep `src/ui/clipboard.hpp` untouched
   - CMake: build `clipboard_macos.mm` only when `APPLE`
3. **Keychain split:**
   - Rename `src/persistence/keychain.cpp` → `src/persistence/keychain_macos.cpp` (if there is no Objective-C, otherwise `.mm`)
   - Keep `src/persistence/keychain.hpp` untouched
   - Add a new function declaration to the header: `keychain::StorageBackend storage_backend();` returning an enum (`MACOS_KEYCHAIN`, `LIBSECRET`, `FILE_FALLBACK`, `UNKNOWN`). macOS implementation returns `MACOS_KEYCHAIN`. UI consumers added later.
   - CMake: build `keychain_macos.cpp` only when `APPLE`
4. **SHA256 split:**
   - Extract the SHA256 function from `src/persistence/model_manifest.cpp` into a new TU `src/persistence/sha256_commoncrypto.cpp` with a small internal-only header `src/persistence/sha256.hpp` exposing `std::string sha256_hex(const void* data, std::size_t len)` (or matching today's signature)
   - `model_manifest.cpp` now calls `sha256::sha256_hex(...)` instead of CommonCrypto directly
   - CMake: build `sha256_commoncrypto.cpp` only when `APPLE`

## Files Affected

- **Renamed:** `mic_permission.mm`, `clipboard.mm`, `keychain.cpp` → `*_macos.{mm,cpp}`
- **New:** `src/persistence/sha256.hpp`, `src/persistence/sha256_commoncrypto.cpp`
- **Modified:** `src/persistence/model_manifest.cpp` (delegate SHA256), `src/persistence/keychain.hpp` (add `storage_backend()` enum + decl)
- **Modified:** `CMakeLists.txt`

## CMake Changes

Each renamed TU gets an `if(APPLE)` gate. The same `if(APPLE)` blocks will receive `else()` branches in Phase 4 — keep them syntactically ready.

## Commit Message

```
refactor: split platform-specific code into per-OS files

Rename mic_permission / clipboard / keychain implementations with _macos
suffix, extract SHA256 into a dedicated TU (sha256_commoncrypto.cpp).
Public headers unchanged. CMake gates each file under if(APPLE). No
behavior change; prepares for Linux companions in M12 Phase 4.

Also adds keychain::storage_backend() enum (returns MACOS_KEYCHAIN here)
for the Settings-tab indicator coming in Phase 5.

Closes M12 Phase 3.
```

## Validation

- `make all` green on macOS — build + format + lint + test
- Plugin loads in X-Plane and behaves identically
- `Log.txt` should be byte-identical at startup (no new log lines from this phase)

## Depends On

- Phase 0 (audit invariant) — protective
- Phases 1–2 independent of this one; can be done in parallel, but easier to land in order

## Estimated Effort

~½ day
