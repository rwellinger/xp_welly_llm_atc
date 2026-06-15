# M12 — Linux Port — Phase 6: Clipboard via xclip / wl-paste

## Context

Cross-platform port on branch `feat/linux-port`. Phase 4 shipped Linux with an empty clipboard stub. The `[Paste]` buttons next to API-key fields therefore do nothing on Linux — testers have to type their (long) OpenAI / Mistral keys by hand. That's painful.

This phase adds a real clipboard read by shelling out to `xclip` (X11) or `wl-paste` (Wayland), chosen at runtime. Same `clipboard::read_text()` signature as on macOS — purely an internal upgrade, no API change. See `feedback_imgui_paste_button` memory: Cmd+V is swallowed by X-Plane on macOS and the equivalent on Linux is unreliable too, so an explicit `[Paste]` button driven by `GetClipboardText()` is the established pattern.

Avoiding native X11 / Wayland linkage keeps the .xpl binary clean and the failure mode obvious (user sees stderr from the subprocess in `Log.txt` instead of a confusing GTK init crash).

## Goal

`clipboard::read_text()` on Linux returns the actual desktop clipboard content. Works on X11 sessions (via `xclip`) and Wayland sessions (via `wl-paste`), with a clear error message if neither tool is present.

## Steps

1. **Session detection:**
   - Check env var `$WAYLAND_DISPLAY` — if set and non-empty, prefer `wl-paste`
   - Otherwise fall back to `xclip -selection clipboard -o`
2. **Subprocess invocation in `clipboard_linux.cpp`:**
   - Use `popen()` with the chosen command + `"r"` mode
   - Read up to a sane cap (4 KB — API keys are ~50 chars, no reason to allow more)
   - `pclose()` and check exit status
   - On non-zero exit, log a one-line warning to `Log.txt` and return empty string
3. **Tool availability check:**
   - On the very first call, check `which wl-paste` / `which xclip` (also via `popen` of `which -s`)
   - If neither found, log once: `[xp_wellys_atc] Clipboard: neither wl-paste nor xclip found, paste disabled`
   - Cache the result statically so we do not re-probe per call
4. **Security:**
   - Do not pass user input to the shell; use hardcoded command strings only
   - Use the `-o` flag explicitly; do not let `popen` interpret extra args
5. **Trim:**
   - Strip trailing newlines that `xclip` / `wl-paste` sometimes append (especially with `-n` not passed)

## Files Affected

- **Modified:** `src/ui/clipboard_linux.cpp` (real implementation replacing stub)

## CMake Changes

None. Subprocess approach needs no new linkage.

## Commit Message

```
feat(clipboard): Linux xclip/wl-paste subprocess support

Replace the Phase 4 stub. Detect session via $WAYLAND_DISPLAY, shell
out to xclip (X11) or wl-paste (Wayland) with strict hardcoded args.
Logs once if neither tool is present; [Paste] buttons silently do
nothing in that case rather than crashing.

Closes M12 Phase 6.
```

## Validation

- macOS unchanged
- Linux tester on X11 (Xorg): `[Paste]` button reads clipboard content into the API-key field
- Linux tester on Wayland (GNOME / KDE Plasma 6 / Sway with wl-clipboard installed): same
- Linux tester without xclip/wl-paste installed: `[Paste]` does nothing, `Log.txt` contains the one-time notice
- Verify long keys (>1 KB nonsense input) are not truncated mid-key and do not crash the plugin

## Depends On

- Phase 4 (clipboard stub exists)

## Estimated Effort

~¼ day
