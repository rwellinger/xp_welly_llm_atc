# START.md

This document is the one-time bootstrapping instruction for Claude Code.
Execute all steps in order. Do not skip ahead. After this is complete, START.md is obsolete.

Read CLAUDE.md completely before starting.

---

## Goal

At the end of this document the project must:
- Have a complete directory structure
- Have a working CMakeLists.txt and Makefile
- Compile cleanly with `make build` (zero warnings, zero errors)
- Load in X-Plane 12 without crashing
- Show a "Welly's ATC" menu entry under Plugins
- Show an empty Dear ImGui window when that menu entry is clicked
- Be committed to a local git repository with a proper .gitignore

This is pure scaffold — no ATC logic, no audio, no API calls.

---

## Step 1 — Git Init

```bash
git init
git checkout -b main
```

Create `.gitignore`:

```
build/
sdk/
vendor/
data/settings.json
*.xpl
*.o
*.a
*.dylib
.DS_Store
CMakeFiles/
CMakeCache.txt
cmake_install.cmake
Makefile.cmake
```

---

## Step 2 — Directory Structure

Create all directories and empty placeholder files exactly as defined in CLAUDE.md under "Directory Structure".

For each `.cpp` / `.hpp` pair in `src/`, create the files with:
- Header: include guard, namespace declaration, empty `init()` and `stop()` stubs
- Source: include its own header, empty `init()` and `stop()` implementations

Do not implement any logic yet — scaffolding only.

---

## Step 3 — CMakeLists.txt

Model this exactly after xp_pilot's CMakeLists.txt pattern. Requirements:

- Project name: `xp_wellys_atc`
- C++17 standard
- Universal binary: `CMAKE_OSX_ARCHITECTURES "arm64;x86_64"`
- `CMAKE_OSX_DEPLOYMENT_TARGET "12.0"`
- Toolchain: Homebrew LLVM at `/opt/homebrew/opt/llvm`
- Include dirs: `sdk/XPLM`, `sdk/XPWidgets`, `vendor/imgui`, `vendor/`
- Sources: all `.cpp` files in `src/`, all Dear ImGui `.cpp` files in `vendor/imgui/`
- Linked frameworks: `AudioToolbox`, `AudioUnit`, `CoreAudio`, `AVFoundation`, `Security`, `OpenGL`
- `find_package(CURL REQUIRED)` — link `CURL::libcurl`
- Plugin output: `xp_wellys_atc.xpl` (shared library, `.xpl` suffix)
- Compiler flags: `-Wall -Wextra -fvisibility=hidden -Wno-deprecated-declarations`
- X-Plane defines: `XPLM200=1 XPLM210=1 XPLM300=1 XPLM301=1 APL=1 IBM=0 LIN=0`

---

## Step 4 — Makefile

```makefile
XPLANE_PLUGINS := $(HOME)/X-Plane\ 12/Resources/plugins

.PHONY: setup build install clean

setup:
	@echo "Downloading X-Plane SDK..."
	@mkdir -p sdk
	# Download and unzip SDK headers (same as xp_pilot setup.sh pattern)
	@echo "Downloading Dear ImGui..."
	@mkdir -p vendor/imgui
	# Download Dear ImGui v1.91.9
	@echo "Downloading nlohmann/json..."
	# Download json.hpp v3.11.3
	@echo "Setup complete."

build:
	@cmake -B build -DCMAKE_BUILD_TYPE=Release -S . 2>&1
	@cmake --build build --parallel 2>&1

install: build
	@mkdir -p $(XPLANE_PLUGINS)/xp_wellys_atc/mac_x64
	@cp build/xp_wellys_atc.xpl $(XPLANE_PLUGINS)/xp_wellys_atc/mac_x64/
	@codesign --force --deep --sign - $(XPLANE_PLUGINS)/xp_wellys_atc/mac_x64/xp_wellys_atc.xpl
	@echo "Installed and signed."

clean:
	@rm -rf build/
```

For the `setup` target: implement the actual download commands using `curl` and `unzip`, mirroring the exact approach from https://github.com/rwellinger/xp_pilot/blob/main/Makefile

---

## Step 5 — main.cpp

Implement the full plugin entry point:

```cpp
// Required X-Plane plugin entry points:
// XPluginStart, XPluginStop, XPluginEnable, XPluginDisable

// On XPluginStart:
//   - Call init() on: settings, xplane_context, atc_ui
//   - Register flight loop callback (xplane_context update + ptt polling)
//   - Create plugin menu "Welly's ATC" with item "Open / Close"

// On XPluginEnable:
//   - Call init() on: ptt_input, atc_session

// On XPluginDisable:
//   - Call stop() on: ptt_input, atc_session

// On XPluginStop:
//   - Unregister flight loop, menu
//   - Call stop() on: atc_ui, xplane_context, settings

// Menu handler: toggle atc_ui window visibility
```

Use `XPLMDebugString` to log `"[xp_wellys_atc] Plugin started\n"` on start.

---

## Step 6 — atc_ui (minimal)

Implement just enough ImGui to show an empty window:

- Window title: "Welly's ATC"
- Toggleable via `atc_ui::toggle()`
- Shows placeholder text: "ATC not yet initialized"
- No tabs, no settings panel yet — that comes in M7

---

## Step 7 — xplane_context (minimal)

Implement DataRef registration and per-frame update for the full `XPlaneContext` struct as defined in CLAUDE.md. All fields must be populated. For `nearest_airport_id` and `is_towered_airport`, stub with empty string and `false` for now — implement properly in M1.

---

## Step 8 — Verify Build

```bash
make setup
make build
```

Expected: zero compiler warnings, zero errors. Output: `build/xp_wellys_atc.xpl`

Fix any issues before proceeding.

---

## Step 9 — Initial Commit

```bash
git add -A
git commit -m "chore: initial scaffold — compiles, loads in X-Plane, shows empty ImGui window"
```

Do not push yet — remote will be added manually by the developer.

---

## Done

The scaffold is complete. The developer will now proceed with MILESTONE_M1.md.
