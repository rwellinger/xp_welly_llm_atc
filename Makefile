SHELL := /bin/bash

# ── Platform detection ────────────────────────────────────────────────────────
OS := $(shell uname -s)

# XPLANE_ROOT can be overridden: `make install XPLANE_ROOT=/path/to/xplane`
ifeq ($(OS),Darwin)
    XPLANE_ROOT ?= /Users/robertw/X-Plane 12
    PLUGIN_ARCH_DIR := mac_x64
    PLUGIN_DIR := $(XPLANE_ROOT)/Resources/available plugins/xp_wellys_atc
else
    XPLANE_ROOT ?= $(HOME)/X-Plane 12
    PLUGIN_ARCH_DIR := lin_x64
    PLUGIN_DIR := $(XPLANE_ROOT)/Resources/plugins/xp_wellys_atc
endif

SDK_SENTINEL    := sdk/XPLM/XPLMPlugin.h
IMGUI_SENTINEL  := vendor/imgui/imgui.h
JSON_SENTINEL   := vendor/json.hpp
CATCH2_SENTINEL := vendor/catch2/catch_amalgamated.hpp

# One sentinel for the three submodule trees (whisper.cpp, llama.cpp,
# Piper). They are all pulled in by a single
# `git submodule update --init --recursive` invocation, so tracking
# only the first one is sufficient — if it's missing, the whole
# submodule init runs and lands all three.
SUBMODULES_SENTINEL := spikes/spike_whisper/third_party/whisper.cpp/CMakeLists.txt

CATCH2_VERSION := 3.7.1

# Sources fed to clang-tidy. Mirrors the per-platform source selection in
# CMakeLists.txt (if(APPLE) / elseif(UNIX AND NOT APPLE) blocks around
# audio_input_*, mic_permission_*, clipboard_*) so we never lint a TU that
# wouldn't even compile on the current host. Without this the macOS lint
# run trips over pulse/error.h (PulseAudio headers absent), and the Linux
# lint run would trip over CoreAudio headers.
ifeq ($(OS),Darwin)
LINT_EXCLUDE := src/audio/audio_input_pulseaudio.cpp src/audio/mic_permission_linux.cpp src/ui/clipboard_linux.cpp
else
LINT_EXCLUDE := src/audio/audio_input_coreaudio.cpp
endif
LINT_SOURCES := $(filter-out $(LINT_EXCLUDE),$(wildcard src/main.cpp src/*/*.cpp))

.PHONY: all help setup build install install-mac install-linux install-data package clean distclean format lint sanitize release release-build cleanup-tags cleanup-branches cleanup-runs repl run-repl test test-unit test-scenarios

.DEFAULT_GOAL := help

all: clean format build lint test

# ── Help ──────────────────────────────────────────────────────────────────────
help:
	@echo "xp_wellys_atc - Makefile targets"
	@echo ""
	@echo "  make                   Show this help (default)"
	@echo "  make all               clean + format + build + lint"
	@echo "  make setup             Init submodules + download X-Plane SDK, Dear ImGui, nlohmann/json, Catch2"
	@echo "  make build             Build universal plugin (arm64 local+cloud, x86_64 cloud-only) -> build/xp_wellys_atc.xpl"
	@echo "  make repl              Build headless CLI -> build/atc_repl"
	@echo "  make run-repl          Build + run the CLI (stdin transcripts)"
	@echo "  make test              Run unit tests + scenario tests"
	@echo "  make test-unit         Build + run Catch2 unit tests"
	@echo "  make test-scenarios    Build + run all scenario tests in testscripts/"
	@echo "  make install           Code-sign and install plugin to X-Plane"
	@echo "  make format            Run clang-format on src/*.cpp src/*.hpp"
	@echo "  make lint              Run clang-tidy on src/*.cpp"
	@echo "  make sanitize          Build atc_repl + tests with ASan+UBSan and run them"
	@echo "  make release VERSION=X Tag and push release (writes VERSION.txt)"
	@echo "  make release-build     Build plugin with RELEASE=ON (embeds VERSION.txt)"
	@echo "  make cleanup-tags      Prune local tags no longer on origin"
	@echo "  make cleanup-branches  Prune local branches whose remote is gone"
	@echo "  make cleanup-runs      Delete all GitHub Actions runs except the newest per workflow"
	@echo "  make clean             Remove build/, build-lint/ and build-sanitize/"
	@echo "  make distclean         clean + remove sdk/ and vendor/ (everything 'make setup' installed)"
	@echo "  make help              Show this help"

# ── Setup ─────────────────────────────────────────────────────────────────────
setup: $(SUBMODULES_SENTINEL) $(SDK_SENTINEL) $(IMGUI_SENTINEL) $(JSON_SENTINEL) $(CATCH2_SENTINEL)
	@echo "Setup complete. Run 'make build' to compile."

$(SUBMODULES_SENTINEL):
	@if [ ! -d .git ]; then \
	    echo "ERROR: not a git checkout - submodules cannot be initialised."; \
	    echo ""; \
	    echo "If you downloaded a release ZIP, the third-party sources"; \
	    echo "(whisper.cpp, llama.cpp, Piper) are not bundled. Re-clone with:"; \
	    echo ""; \
	    echo "    git clone --recurse-submodules <repo-url>"; \
	    echo ""; \
	    exit 1; \
	fi
	@echo "Initialising git submodules (whisper.cpp, llama.cpp, Piper)..."
	@git submodule update --init --recursive
	@echo "Submodules ready."

$(SDK_SENTINEL):
	@echo "Downloading X-Plane SDK..."
	@set -euo pipefail; \
	TMP=$$(mktemp -d); \
	trap "rm -rf $$TMP" EXIT; \
	curl -fsSL "https://developer.x-plane.com/wp-content/plugins/code-sample-generation/sdk_zip_files/XPSDK430.zip" \
	     -o "$$TMP/sdk.zip"; \
	unzip -q "$$TMP/sdk.zip" -d "$$TMP/sdk_extracted"; \
	mkdir -p sdk/XPLM sdk/XPWidgets sdk/Libraries/Win sdk/Libraries/Mac sdk/Libraries/Lin; \
	find "$$TMP/sdk_extracted" -path "*/CHeaders/XPLM/*.h"    -exec cp {} sdk/XPLM/ \;; \
	find "$$TMP/sdk_extracted" -path "*/CHeaders/Widgets/*.h"  -exec cp {} sdk/XPWidgets/ \;; \
	find "$$TMP/sdk_extracted" -path "*/Libraries/Win/*.lib"   -exec cp {} sdk/Libraries/Win/ \;; \
	find "$$TMP/sdk_extracted" -path "*/Libraries/Lin/*.so"    -exec cp {} sdk/Libraries/Lin/ \;; \
	cp -R "$$TMP/sdk_extracted"/*/Libraries/Mac/*.framework sdk/Libraries/Mac/ 2>/dev/null || \
	find "$$TMP/sdk_extracted" -name "*.framework" -exec cp -R {} sdk/Libraries/Mac/ \;
	@echo "SDK headers installed."

$(IMGUI_SENTINEL):
	@echo "Downloading Dear ImGui v1.91.9..."
	@set -euo pipefail; \
	TMP=$$(mktemp -d); \
	trap "rm -rf $$TMP" EXIT; \
	mkdir -p vendor/imgui/backends; \
	curl -fsSL "https://github.com/ocornut/imgui/archive/refs/tags/v1.91.9.zip" -o "$$TMP/imgui.zip"; \
	unzip -q "$$TMP/imgui.zip" -d "$$TMP/"; \
	SRC="$$TMP/imgui-1.91.9"; \
	cp "$$SRC"/imgui.{h,cpp} vendor/imgui/; \
	cp "$$SRC"/imgui_{draw,tables,widgets}.cpp vendor/imgui/; \
	cp "$$SRC"/imgui_internal.h "$$SRC"/imconfig.h vendor/imgui/; \
	cp "$$SRC"/imstb_textedit.h "$$SRC"/imstb_rectpack.h "$$SRC"/imstb_truetype.h vendor/imgui/ 2>/dev/null || true; \
	cp "$$SRC"/backends/imgui_impl_opengl2.{h,cpp} vendor/imgui/backends/
	@echo "Dear ImGui installed."

$(JSON_SENTINEL):
	@echo "Downloading nlohmann/json v3.11.3..."
	@mkdir -p vendor
	@curl -fsSL "https://github.com/nlohmann/json/releases/download/v3.11.3/json.hpp" \
	     -o vendor/json.hpp
	@echo "nlohmann/json installed."

$(CATCH2_SENTINEL):
	@echo "Downloading Catch2 v$(CATCH2_VERSION) (amalgamated)..."
	@set -euo pipefail; \
	TMP=$$(mktemp -d); \
	trap "rm -rf $$TMP" EXIT; \
	mkdir -p vendor/catch2; \
	curl -fsSL "https://github.com/catchorg/Catch2/archive/refs/tags/v$(CATCH2_VERSION).tar.gz" \
	     -o "$$TMP/catch2.tar.gz"; \
	tar -xzf "$$TMP/catch2.tar.gz" -C "$$TMP/"; \
	cp "$$TMP/Catch2-$(CATCH2_VERSION)/extras/catch_amalgamated.hpp" vendor/catch2/; \
	cp "$$TMP/Catch2-$(CATCH2_VERSION)/extras/catch_amalgamated.cpp" vendor/catch2/
	@echo "Catch2 installed."

# ── Build ─────────────────────────────────────────────────────────────────────
# Always produces a universal xp_wellys_atc.xpl that contains both an
# arm64 slice (whisper.cpp + llama.cpp + Piper + OpenAI) and an x86_64
# slice (OpenAI only — Metal + the onnxruntime prebuilt are Apple
# Silicon only). The two slices share src/ and CMakeLists.txt but
# differ via -DXPWELLYS_USE_LOCAL_INFERENCE.
#
# Strategy: two separate CMake configures (build-arm64/, build-x86_64/),
# each producing its own .xpl, then lipo-merged into build/xp_wellys_atc.xpl.
# The arm64 slice ships libpiper.dylib + libonnxruntime.dylib next to the
# .xpl so they're picked up by @loader_path; the x86_64 slice has no
# such dylibs to ship.
#
# `make install` copies build/xp_wellys_atc.xpl into the plugin dir
# together with the staged dylibs.
# `RELEASE_FLAG` is passed through to both CMake configure calls so the
# GitHub Actions workflow can flip `-DRELEASE=ON` for tag-driven
# release builds without duplicating the logic. Empty by default
# (regular dev build); `release-build` sets it to `-DRELEASE=ON`.
RELEASE_FLAG ?=

build: $(SUBMODULES_SENTINEL) $(SDK_SENTINEL) $(IMGUI_SENTINEL) $(JSON_SENTINEL) $(CATCH2_SENTINEL)
ifeq ($(OS),Darwin)
	@echo "=== Building universal xp_wellys_atc (arm64 local+cloud, x86_64 cloud-only) ==="
	@echo ""
	@echo "--- arm64 slice (local + cloud) ---"
	cmake -B build-arm64 -DCMAKE_BUILD_TYPE=Release $(RELEASE_FLAG) \
	    -DCMAKE_OSX_ARCHITECTURES=arm64 \
	    -DXPWELLYS_USE_LOCAL_INFERENCE=ON \
	    -DBUILD_TESTS=OFF \
	    -Wno-dev
	cmake --build build-arm64 --parallel
	@echo ""
	@echo "--- x86_64 slice (cloud-only) ---"
	cmake -B build-x86_64 -DCMAKE_BUILD_TYPE=Release $(RELEASE_FLAG) \
	    -DCMAKE_OSX_ARCHITECTURES=x86_64 \
	    -DXPWELLYS_USE_LOCAL_INFERENCE=OFF \
	    -DBUILD_TESTS=OFF \
	    -Wno-dev
	cmake --build build-x86_64 --parallel
	@echo ""
	@echo "--- lipo merge ---"
	@mkdir -p build
	lipo -create \
	    build-arm64/xp_wellys_atc.xpl \
	    build-x86_64/xp_wellys_atc.xpl \
	    -output build/xp_wellys_atc.xpl
	@cp build-arm64/libpiper.dylib              build/
	@cp build-arm64/libonnxruntime.1.22.0.dylib build/
	@cp build-arm64/libonnxruntime.dylib        build/
	@cp -R build-arm64/espeak_ng-install        build/ 2>/dev/null || true
	@echo ""
	@file build/xp_wellys_atc.xpl
	@lipo -info build/xp_wellys_atc.xpl
	@echo "Done. Run 'make install' to deploy the universal .xpl."
else
	@echo "=== Building xp_wellys_atc for Linux ==="
	cmake -B build -DCMAKE_BUILD_TYPE=Release $(RELEASE_FLAG) \
	    -DXPWELLYS_USE_LOCAL_INFERENCE=ON \
	    -DBUILD_TESTS=OFF \
	    -Wno-dev
	cmake --build build --parallel
	@echo ""
	@file build/xp_wellys_atc.xpl
	@echo "Done. Run 'make install' to deploy."
endif

# ── REPL (headless CLI) ───────────────────────────────────────────────────────
repl: $(SUBMODULES_SENTINEL) $(SDK_SENTINEL) $(IMGUI_SENTINEL) $(JSON_SENTINEL) $(CATCH2_SENTINEL)
	@echo "=== Building atc_repl ==="
	cmake -B build -DCMAKE_BUILD_TYPE=Release -Wno-dev
	cmake --build build --target atc_repl --parallel
	@echo ""
	@file build/atc_repl
	@echo "Done. Run 'make run-repl' or './build/atc_repl'."

run-repl: repl
	./build/atc_repl

# ── Tests ─────────────────────────────────────────────────────────────────────
test: test-unit test-scenarios

test-unit: $(SUBMODULES_SENTINEL) $(SDK_SENTINEL) $(IMGUI_SENTINEL) $(JSON_SENTINEL) $(CATCH2_SENTINEL)
	@echo "=== Building xp_wellys_atc unit tests ==="
	cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON -Wno-dev
	cmake --build build --target xp_wellys_atc_tests --parallel
	@echo ""
	@echo "=== Running unit tests ==="
	@./build/xp_wellys_atc_tests

test-scenarios: repl
	@echo "=== Running scenario tests ==="
	# Non-recursive glob: testscripts/experimental_ifr/ is intentionally
	# excluded — those IFR scenarios are quarantined (see that dir's README).
	./build/atc_repl run testscripts/*.json

# Quarantined IFR departure scenarios — currently failing against the merged
# IFR feature code (PR #11). Not part of `make test`. See
# testscripts/experimental_ifr/README.md for the known root causes.
test-scenarios-ifr: repl
	@echo "=== Running quarantined IFR scenario tests ==="
	./build/atc_repl run testscripts/experimental_ifr/*.json

# ── Install ───────────────────────────────────────────────────────────────────
# `install` is a thin dispatcher that routes to the per-platform target.
# `install-mac` and `install-linux` handle binary placement + platform-
# specific post-processing (codesign / rpath on macOS, .so copies on
# Linux), then chain into `install-data` for the platform-independent
# data/* and Resources/* bundle.
install:
ifeq ($(OS),Darwin)
	@$(MAKE) --no-print-directory install-mac
else
	@$(MAKE) --no-print-directory install-linux
endif

install-mac:
	@if [ ! -f "build/xp_wellys_atc.xpl" ]; then \
	    echo "Plugin not built yet. Run 'make build' first."; exit 1; \
	fi
	@echo "=== Installing xp_wellys_atc (macOS) ==="
	@mkdir -p "$(PLUGIN_DIR)/$(PLUGIN_ARCH_DIR)"
	@cp build/xp_wellys_atc.xpl "$(PLUGIN_DIR)/$(PLUGIN_ARCH_DIR)/"
	@cp build/libpiper.dylib              "$(PLUGIN_DIR)/$(PLUGIN_ARCH_DIR)/"
	@cp build/libonnxruntime.1.22.0.dylib "$(PLUGIN_DIR)/$(PLUGIN_ARCH_DIR)/"
	@cp build/libonnxruntime.dylib        "$(PLUGIN_DIR)/$(PLUGIN_ARCH_DIR)/"
	@xattr -dr com.apple.quarantine "$(PLUGIN_DIR)/$(PLUGIN_ARCH_DIR)/xp_wellys_atc.xpl"           2>/dev/null || true
	@xattr -dr com.apple.quarantine "$(PLUGIN_DIR)/$(PLUGIN_ARCH_DIR)/libpiper.dylib"              2>/dev/null || true
	@xattr -dr com.apple.quarantine "$(PLUGIN_DIR)/$(PLUGIN_ARCH_DIR)/libonnxruntime.1.22.0.dylib" 2>/dev/null || true
	@for rp in $$(otool -l "$(PLUGIN_DIR)/$(PLUGIN_ARCH_DIR)/xp_wellys_atc.xpl" \
	    | awk '/LC_RPATH/{flag=1; next} flag && /path/ {print $$2; flag=0}'); do \
	    install_name_tool -delete_rpath "$$rp" "$(PLUGIN_DIR)/$(PLUGIN_ARCH_DIR)/xp_wellys_atc.xpl" 2>/dev/null || true; \
	done
	@install_name_tool -add_rpath "@loader_path" "$(PLUGIN_DIR)/$(PLUGIN_ARCH_DIR)/xp_wellys_atc.xpl"
	@codesign --force --deep --sign - "$(PLUGIN_DIR)/$(PLUGIN_ARCH_DIR)/libonnxruntime.1.22.0.dylib"
	@codesign --force --deep --sign - "$(PLUGIN_DIR)/$(PLUGIN_ARCH_DIR)/libpiper.dylib"
	@codesign --force --deep --sign - "$(PLUGIN_DIR)/$(PLUGIN_ARCH_DIR)/xp_wellys_atc.xpl"
	@$(MAKE) --no-print-directory install-data

install-linux:
	@if [ ! -f "build/xp_wellys_atc.xpl" ]; then \
	    echo "Plugin not built yet. Run 'make build' first."; exit 1; \
	fi
	@echo "=== Installing xp_wellys_atc (Linux) ==="
	@mkdir -p "$(PLUGIN_DIR)/$(PLUGIN_ARCH_DIR)"
	@cp build/xp_wellys_atc.xpl "$(PLUGIN_DIR)/$(PLUGIN_ARCH_DIR)/"
	@if [ -f "build/libpiper.so" ]; then \
	    cp build/libpiper.so "$(PLUGIN_DIR)/$(PLUGIN_ARCH_DIR)/"; \
	fi
	@if [ -f "build/libonnxruntime.so.1.22.0" ]; then \
	    cp build/libonnxruntime.so.1.22.0 "$(PLUGIN_DIR)/$(PLUGIN_ARCH_DIR)/"; \
	    cp build/libonnxruntime.so        "$(PLUGIN_DIR)/$(PLUGIN_ARCH_DIR)/"; \
	    ln -sf libonnxruntime.so.1.22.0   "$(PLUGIN_DIR)/$(PLUGIN_ARCH_DIR)/libonnxruntime.so.1"; \
	fi
	@if [ -f "build/libonnxruntime_providers_shared.so" ]; then \
	    cp build/libonnxruntime_providers_shared.so "$(PLUGIN_DIR)/$(PLUGIN_ARCH_DIR)/"; \
	fi
	@$(MAKE) --no-print-directory install-data

install-data:
	@# Bundle espeak-ng-data (~19 MB) inside the plugin so Piper's
	@# phonemizer finds its dictionary at runtime via the plugin-relative
	@# path resolved by model_paths::espeakng_data_dir(). Models live in
	@# Resources/models/ and are downloaded by the user on first launch
	@# (P5); espeak-ng-data is part of the .xpl bundle, NOT downloaded.
	@if [ -d "build/espeak_ng-install/share/espeak-ng-data" ]; then \
	    mkdir -p "$(PLUGIN_DIR)/Resources/espeak-ng-data"; \
	    rsync -a --delete \
	        "build/espeak_ng-install/share/espeak-ng-data/" \
	        "$(PLUGIN_DIR)/Resources/espeak-ng-data/"; \
	    echo "Installed: $(PLUGIN_DIR)/Resources/espeak-ng-data/"; \
	else \
	    echo "WARNING: build/espeak_ng-install/share/espeak-ng-data missing — run make build first"; \
	fi
	@# Models live under Resources/models/. Created empty here so the
	@# in-plugin downloader has a target dir on first launch even
	@# before the user has downloaded anything.
	@mkdir -p "$(PLUGIN_DIR)/Resources/models"
	@mkdir -p "$(PLUGIN_DIR)/data"
	@if [ ! -f "$(PLUGIN_DIR)/data/settings.json" ]; then \
	    cp data/settings.json "$(PLUGIN_DIR)/data/"; \
	    echo "Installed: $(PLUGIN_DIR)/data/settings.json"; \
	else \
	    echo "Kept existing settings.json"; \
	fi
	@cp data/atc_prompt_templates.json "$(PLUGIN_DIR)/data/"
	@echo "Installed: $(PLUGIN_DIR)/data/atc_prompt_templates.json"
	@# Always overwrite the models catalog — slugs and Piper voice
	@# hashes are baked into the bundled JSON, and a stale user copy
	@# (e.g. carried over from a pre-catalog install) would silently
	@# point the loader at outdated entries. Per-user customization
	@# happens via Settings, not by hand-editing this file.
	@cp data/models_catalog.json "$(PLUGIN_DIR)/data/"
	@echo "Installed: $(PLUGIN_DIR)/data/models_catalog.json"
	@mkdir -p "$(PLUGIN_DIR)/data/atc_profiles/eu/vfr" \
	          "$(PLUGIN_DIR)/data/atc_profiles/eu/ifr" \
	          "$(PLUGIN_DIR)/data/atc_profiles/us"
	@cp data/atc_profiles/eu/vfr/atc_templates.json  "$(PLUGIN_DIR)/data/atc_profiles/eu/vfr/"
	@cp data/atc_profiles/eu/vfr/flight_rules.json   "$(PLUGIN_DIR)/data/atc_profiles/eu/vfr/"
	@cp data/atc_profiles/eu/ifr/atc_templates.json  "$(PLUGIN_DIR)/data/atc_profiles/eu/ifr/"
	@cp data/atc_profiles/eu/ifr/flight_rules.json   "$(PLUGIN_DIR)/data/atc_profiles/eu/ifr/"
	@cp data/atc_profiles/eu/intent_rules.json       "$(PLUGIN_DIR)/data/atc_profiles/eu/"
	@cp data/atc_profiles/eu/phraseology_hints.json  "$(PLUGIN_DIR)/data/atc_profiles/eu/"
	@cp data/atc_profiles/eu/ui_strings.json         "$(PLUGIN_DIR)/data/atc_profiles/eu/"
	@echo "Installed: $(PLUGIN_DIR)/data/atc_profiles/eu/ (vfr/ + ifr/ + shared)"
	@cp data/atc_profiles/us/atc_templates.json     "$(PLUGIN_DIR)/data/atc_profiles/us/"
	@cp data/atc_profiles/us/flight_rules.json      "$(PLUGIN_DIR)/data/atc_profiles/us/"
	@cp data/atc_profiles/us/intent_rules.json      "$(PLUGIN_DIR)/data/atc_profiles/us/"
	@cp data/atc_profiles/us/phraseology_hints.json "$(PLUGIN_DIR)/data/atc_profiles/us/"
	@cp data/atc_profiles/us/ui_strings.json        "$(PLUGIN_DIR)/data/atc_profiles/us/"
	@echo "Installed: $(PLUGIN_DIR)/data/atc_profiles/us/*.json"
	@mkdir -p "$(PLUGIN_DIR)/data/vrps"
	@cp data/vrps/airport_vrps.json "$(PLUGIN_DIR)/data/vrps/"
	@echo "Installed: $(PLUGIN_DIR)/data/vrps/airport_vrps.json"
	@# Cleanup of legacy paths from pre-Phase-B installs (region-scoped
	@# data + top-level JSONs from the era before the regions folder).
	@rm -f "$(PLUGIN_DIR)/data/atc_templates.json" \
	       "$(PLUGIN_DIR)/data/flight_rules.json" \
	       "$(PLUGIN_DIR)/data/airport_vrps.json"
	@rm -rf "$(PLUGIN_DIR)/data/regions"
	@echo "Installed and signed."

# ── Package ───────────────────────────────────────────────────────────────────
# Produces a self-contained ZIP ready to drop into X-Plane's
# Resources/plugins/ directory.  On Linux the archive is
# xp_wellys_atc-linux-<version>.zip; on macOS xp_wellys_atc-mac-<version>.zip.
# Usage: make package   (VERSION read from VERSION.txt if not overridden)
DIST_VERSION := $(shell cat VERSION.txt 2>/dev/null | tr -d '[:space:]')
ifeq ($(DIST_VERSION),)
    DIST_VERSION := dev
endif
DIST_GIT_HASH := $(shell git rev-parse --short HEAD 2>/dev/null)
ifeq ($(OS),Darwin)
    DIST_PLATFORM := mac
    DIST_EXT      := zip
else
    DIST_PLATFORM := linux
    DIST_EXT      := tar.gz
endif
# Build counter: increments each time package is run on the same commit,
# so successive dev builds on the same hash get -1, -2, -3, ...
_DIST_BASE    := xp_wellys_atc-$(DIST_PLATFORM)-$(DIST_VERSION)-$(DIST_GIT_HASH)
DIST_BUILD_N  := $(shell n=$$(ls dist/$(_DIST_BASE)-*.$(DIST_EXT) 2>/dev/null | wc -l); echo $$((n + 1)))
DIST_NAME     := $(_DIST_BASE)-$(DIST_BUILD_N)
DIST_STAGE    := dist/$(DIST_NAME)/xp_wellys_atc

package:
	@if [ ! -f "build/xp_wellys_atc.xpl" ]; then \
	    echo "Plugin not built. Run 'make build' first."; exit 1; \
	fi
	@echo "=== Packaging xp_wellys_atc $(DIST_VERSION) for $(DIST_PLATFORM) ==="
	@rm -rf dist/$(DIST_NAME)
	@mkdir -p "$(DIST_STAGE)/$(PLUGIN_ARCH_DIR)"
	@# ── Binaries ──
	@cp build/xp_wellys_atc.xpl "$(DIST_STAGE)/$(PLUGIN_ARCH_DIR)/"
ifeq ($(OS),Darwin)
	@[ -f build-arm64/libpiper.dylib ]             && cp build-arm64/libpiper.dylib "$(DIST_STAGE)/$(PLUGIN_ARCH_DIR)/" || true
	@[ -f build-arm64/libonnxruntime.1.22.0.dylib ]&& cp build-arm64/libonnxruntime.1.22.0.dylib "$(DIST_STAGE)/$(PLUGIN_ARCH_DIR)/" || true
	@[ -f build-arm64/libonnxruntime.dylib ]       && cp build-arm64/libonnxruntime.dylib "$(DIST_STAGE)/$(PLUGIN_ARCH_DIR)/" || true
else
	@[ -f build/libpiper.so ]                           && cp build/libpiper.so "$(DIST_STAGE)/$(PLUGIN_ARCH_DIR)/" || true
	@[ -f build/libonnxruntime.so.1.22.0 ]             && cp build/libonnxruntime.so.1.22.0 "$(DIST_STAGE)/$(PLUGIN_ARCH_DIR)/" || true
	@[ -f build/libonnxruntime.so ]                    && cp build/libonnxruntime.so "$(DIST_STAGE)/$(PLUGIN_ARCH_DIR)/" || true
	@[ -f build/libonnxruntime_providers_shared.so ]   && cp build/libonnxruntime_providers_shared.so "$(DIST_STAGE)/$(PLUGIN_ARCH_DIR)/" || true
	@cd "$(DIST_STAGE)/$(PLUGIN_ARCH_DIR)" && \
	    [ -f libonnxruntime.so.1.22.0 ] && ln -sf libonnxruntime.so.1.22.0 libonnxruntime.so.1 || true
endif
	@# ── espeak-ng-data ──
	@if [ -d "build/espeak_ng-install/share/espeak-ng-data" ]; then \
	    mkdir -p "$(DIST_STAGE)/Resources/espeak-ng-data"; \
	    rsync -a "build/espeak_ng-install/share/espeak-ng-data/" \
	             "$(DIST_STAGE)/Resources/espeak-ng-data/"; \
	else \
	    echo "WARNING: espeak-ng-data missing — run 'make build' first"; \
	fi
	@mkdir -p "$(DIST_STAGE)/Resources/models"
	@# ── Data files ──
	@mkdir -p "$(DIST_STAGE)/data/atc_profiles/eu/vfr" \
	          "$(DIST_STAGE)/data/atc_profiles/eu/ifr" \
	          "$(DIST_STAGE)/data/atc_profiles/us" \
	          "$(DIST_STAGE)/data/vrps"
	@cp data/atc_prompt_templates.json   "$(DIST_STAGE)/data/"
	@cp data/models_catalog.json         "$(DIST_STAGE)/data/"
	@cp data/vrps/airport_vrps.json      "$(DIST_STAGE)/data/vrps/"
	@cp data/atc_profiles/eu/vfr/atc_templates.json  "$(DIST_STAGE)/data/atc_profiles/eu/vfr/"
	@cp data/atc_profiles/eu/vfr/flight_rules.json   "$(DIST_STAGE)/data/atc_profiles/eu/vfr/"
	@cp data/atc_profiles/eu/ifr/atc_templates.json  "$(DIST_STAGE)/data/atc_profiles/eu/ifr/"
	@cp data/atc_profiles/eu/ifr/flight_rules.json   "$(DIST_STAGE)/data/atc_profiles/eu/ifr/"
	@cp data/atc_profiles/eu/intent_rules.json       "$(DIST_STAGE)/data/atc_profiles/eu/"
	@cp data/atc_profiles/eu/phraseology_hints.json  "$(DIST_STAGE)/data/atc_profiles/eu/"
	@cp data/atc_profiles/eu/ui_strings.json         "$(DIST_STAGE)/data/atc_profiles/eu/"
	@cp data/atc_profiles/us/atc_templates.json      "$(DIST_STAGE)/data/atc_profiles/us/"
	@cp data/atc_profiles/us/flight_rules.json       "$(DIST_STAGE)/data/atc_profiles/us/"
	@cp data/atc_profiles/us/intent_rules.json       "$(DIST_STAGE)/data/atc_profiles/us/"
	@cp data/atc_profiles/us/phraseology_hints.json  "$(DIST_STAGE)/data/atc_profiles/us/"
	@cp data/atc_profiles/us/ui_strings.json         "$(DIST_STAGE)/data/atc_profiles/us/"
ifeq ($(OS),Darwin)
	@# ── macOS: ZIP (-y preserves symlinks) ──
	@cd dist && zip -qry $(DIST_NAME).zip $(DIST_NAME)/
	@echo "Package: dist/$(DIST_NAME).zip"
	@echo "Drop the xp_wellys_atc/ folder inside the ZIP into:"
	@echo "  <X-Plane 12>/Resources/plugins/"
else
	@# ── Linux: tar.gz (symlinks preserved natively) ──
	@cd dist && tar -czf $(DIST_NAME).tar.gz $(DIST_NAME)/
	@echo "Package: dist/$(DIST_NAME).tar.gz"
	@echo "Extract with: tar -xzf $(DIST_NAME).tar.gz -C '<X-Plane 12>/Resources/plugins/'"
endif

# ── Lint ──────────────────────────────────────────────────────────────────────
format:
	@command -v clang-format >/dev/null 2>&1 || { \
	    echo "clang-format not found. Install with: brew install llvm"; \
	    echo "Then add to PATH: export PATH=\"$$(brew --prefix llvm)/bin:$$PATH\""; \
	    exit 1; }
	clang-format -i src/main.cpp src/*/*.cpp src/*/*.hpp

lint: $(SUBMODULES_SENTINEL) $(SDK_SENTINEL) $(IMGUI_SENTINEL) $(JSON_SENTINEL) $(CATCH2_SENTINEL)
	@command -v clang-tidy >/dev/null 2>&1 || { \
	    echo "clang-tidy not found. Install with: brew install llvm"; \
	    echo "Then add to PATH: export PATH=\"$$(brew --prefix llvm)/bin:$$PATH\""; \
	    exit 1; }
	cmake -B build-lint -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_OSX_ARCHITECTURES=arm64 -Wno-dev
	clang-tidy -p build-lint --extra-arg="-isysroot" --extra-arg="$(shell xcrun --show-sdk-path)" $(LINT_SOURCES)

# ── Sanitize ──────────────────────────────────────────────────────────────────
# AddressSanitizer + UBSan on the SDK-free engine OBJECT lib + atc_repl +
# Catch2 tests. The plugin module (`xp_wellys_atc.xpl`) is NOT instrumented —
# ASan inside the X-Plane process is fragile on macOS ARM64. For runtime
# leaks in the live plugin use Instruments.app (Leaks / Allocations
# templates) attached to the X-Plane process.
#
# Findings abort with a non-zero exit (`-fno-sanitize-recover=all`), so this
# target is CI-friendly. Build dir is `build-sanitize/` — independent of
# `build/` so Release artifacts stay untouched.
sanitize: $(SUBMODULES_SENTINEL) $(SDK_SENTINEL) $(IMGUI_SENTINEL) $(JSON_SENTINEL) $(CATCH2_SENTINEL)
	@echo "=== Configuring sanitizer build (ASan + UBSan) ==="
	cmake -B build-sanitize -DCMAKE_BUILD_TYPE=Debug -DXP_WELLYS_ATC_SANITIZE=ON -Wno-dev
	@echo "=== Building atc_repl + xp_wellys_atc_tests with ASan + UBSan ==="
	cmake --build build-sanitize --target atc_repl xp_wellys_atc_tests --parallel
	@echo ""
	@echo "=== Running unit tests under ASan + UBSan ==="
	@ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:print_stacktrace=1 \
	 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
	     ./build-sanitize/xp_wellys_atc_tests
	@echo ""
	@echo "=== Running scenario tests under ASan + UBSan ==="
	@ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:print_stacktrace=1 \
	 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
	     ./build-sanitize/atc_repl run testscripts/*.json
	@echo ""
	@echo "Sanitizer run clean."

# ── Release ───────────────────────────────────────────────────────────────────
release:
	@if [ -z "$(VERSION)" ]; then \
	    echo "Usage: make release VERSION=1.2.1"; exit 1; \
	fi
	@if ! git diff --quiet || ! git diff --cached --quiet; then \
	    echo "Uncommitted changes present. Commit or stash first."; exit 1; \
	fi
	@if [ -n "$$(git ls-files --others --exclude-standard)" ]; then \
	    echo "Untracked files present. Commit or clean up first."; exit 1; \
	fi
	@echo "$(VERSION)" > VERSION.txt
	@git add VERSION.txt
	@git commit -m "release $(VERSION)"
	@git push origin main
	@git tag -a "v$(VERSION)" -m "Release $(VERSION)"
	@git push origin "v$(VERSION)"
	@echo "Released v$(VERSION) and pushed tag to origin."

release-build:
	@$(MAKE) build RELEASE_FLAG=-DRELEASE=ON
	@echo "Done. Universal release build with version from VERSION.txt."

# ── Cleanup Tags ──────────────────────────────────────────────────────────────
cleanup-tags:
	git fetch --prune --prune-tags origin
	@echo "Local tags synced with remote."

# ── Cleanup Branches ──────────────────────────────────────────────────────────
cleanup-branches:
	@echo "Pruning remote-tracking references..."
	@git fetch --prune origin
	@echo ""
	@echo "Local branches whose upstream is gone:"
	@STALE=$$(git for-each-ref --format '%(refname:short) %(upstream:track)' refs/heads | awk '$$2 == "[gone]" {print $$1}'); \
	if [ -z "$$STALE" ]; then \
	    echo "  (none)"; \
	else \
	    echo "$$STALE" | sed 's/^/  /'; \
	    echo ""; \
	    echo "$$STALE" | xargs -n1 git branch -d; \
	fi
	@echo "Local branches synced with remote."

# ── Cleanup GitHub Actions Runs ───────────────────────────────────────────────
cleanup-runs:
	@command -v gh >/dev/null 2>&1 || { \
	    echo "gh not found. Install with: brew install gh"; exit 1; }
	@echo "Deleting GitHub Actions runs (keeping newest per workflow)..."
	@for wf in $$(gh workflow list --json id -q '.[].id'); do \
	    gh run list --workflow=$$wf --limit 1000 --json databaseId -q '.[1:] | .[].databaseId' \
	        | xargs -I {} gh run delete {}; \
	done
	@echo "Cleanup complete."

# ── Clean ─────────────────────────────────────────────────────────────────────
clean:
	rm -rf build/ build-lint/ build-sanitize/ build-arm64/ build-x86_64/

# ── Distclean ─────────────────────────────────────────────────────────────────
# Remove everything 'make setup' downloaded so a full re-bootstrap is forced.
distclean: clean
	rm -rf sdk/ vendor/
	@echo "Removed sdk/ and vendor/. Run 'make setup' to re-download dependencies."
