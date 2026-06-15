# M12 — Linux Port — Phase 5: libsecret Keychain + UI Storage Indicator

## Context

Cross-platform port on branch `feat/linux-port`. Phase 4 landed a Linux build with a **plaintext file** keychain (mode 0600). That works but loses the OS-level encryption macOS users get from `Security.framework`. On 90%+ of Linux desktops there is a Secret Service daemon (GNOME Keyring, KWallet) reachable via D-Bus through `libsecret-1`.

This phase upgrades the Linux keychain to a tiered approach: try libsecret first, fall back to the existing file path if no D-Bus secret daemon answers. The UI surfaces which backend is currently in use — being honest about plaintext fallback is a hard requirement (see `feedback_realism_first` memory: surface the truth, do not silently massage it).

## Goal

Linux keychain attempts libsecret first; falls back gracefully to the Phase 4 file path when libsecret is unavailable. UI Settings tab shows the active storage backend below the `[Save Key]` buttons with an explicit warning if file-fallback is in use.

## Steps

1. **libsecret dependency:**
   - CMake: `pkg_check_modules(LIBSECRET libsecret-1)` — mark optional (`QUIET` rather than `REQUIRED`)
   - Compile-time `#if HAVE_LIBSECRET` gate so a tester missing the dev headers can still build
2. **`keychain_linux.cpp` extension:**
   - At first `save()` or `load()` call after `init()`, probe libsecret: try a no-op `secret_service_get_sync()` with a short timeout (1–2 s)
   - If probe succeeds, all future calls use `secret_password_store_sync` / `secret_password_lookup_sync` / `secret_password_clear_sync` against the schema `(service, account)`
   - If probe fails (no D-Bus, no daemon, headless session), fall through to the file-based path from Phase 4
   - Cache the decision in a static atomic — do not re-probe on every call
   - `storage_backend()` returns `LIBSECRET` or `FILE_FALLBACK` accordingly
3. **UI Settings tab:**
   - In `src/ui/atc_ui.cpp`, below each `[Save Key]` / `[Delete Key]` button row, render a single line:
     - macOS: `Stored in macOS Keychain`
     - Linux + libsecret OK: `Stored in System Keyring (libsecret)`
     - Linux fallback: `WARNING: Stored in file (~/.config/xp_wellys_atc/credentials.json, owner-readable) - no system keyring detected` — render in the warning text color used elsewhere
   - Use `keychain::storage_backend()` to drive the message
   - ASCII-only — no Umlaute, no special chars (CLAUDE.md hard rule, see `feedback_xplane_log_ascii` memory)
4. **Audit logging:**
   - When a save happens, log which backend was used: `[xp_wellys_atc] Keychain save (service=com.xp_wellys_atc.openai backend=libsecret) ok`
   - Still only the last 4 chars of the secret are logged anywhere

## Files Affected

- **Modified:** `src/persistence/keychain_linux.cpp` (libsecret-first tier added)
- **Modified:** `src/ui/atc_ui.cpp` (storage indicator render)
- **Modified:** `CMakeLists.txt` (optional libsecret detection)

## CMake Changes

Add optional libsecret detection on Linux. Define `HAVE_LIBSECRET` when found.

## Commit Message

```
feat(keychain): Linux libsecret with file fallback + UI storage indicator

Try libsecret first (Secret Service via D-Bus). Fall back to the
0600 plaintext file if no secret daemon answers. Settings tab now
shows the active backend; the file-fallback case renders an explicit
"no system keyring detected" warning — honest about plaintext.

Closes M12 Phase 5.
```

## Validation

- macOS: UI shows `Stored in macOS Keychain`, no behavior change
- Linux beta tester on GNOME/KDE: UI shows `Stored in System Keyring (libsecret)`, save+load roundtrips work
- Linux beta tester on headless / Steam-Deck-Gaming-Mode: UI shows the file-fallback warning, save+load still works against the file
- Tester is asked to manually stop gnome-keyring-daemon and confirm graceful fallback on next plugin reload

## Depends On

- Phase 4 (file keychain + `storage_backend()` enum)

## Estimated Effort

~½ day code + tester iteration
