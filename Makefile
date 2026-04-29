SHELL := /bin/bash

XPLANE_ROOT := /Users/robertw/X-Plane 12
PLUGIN_DIR  := $(XPLANE_ROOT)/Resources/available plugins/xp_wellys_atc

SDK_SENTINEL    := sdk/XPLM/XPLMPlugin.h
IMGUI_SENTINEL  := vendor/imgui/imgui.h
JSON_SENTINEL   := vendor/json.hpp
CATCH2_SENTINEL := vendor/catch2/catch_amalgamated.hpp

CATCH2_VERSION := 3.7.1

.PHONY: all help setup build install clean distclean format lint release release-build cleanup-tags cleanup-branches cleanup-runs repl run-repl test test-unit test-scenarios

.DEFAULT_GOAL := help

all: clean format build lint test

# ── Help ──────────────────────────────────────────────────────────────────────
help:
	@echo "xp_wellys_atc - Makefile targets"
	@echo ""
	@echo "  make                   Show this help (default)"
	@echo "  make all               clean + format + build + lint"
	@echo "  make setup             Download X-Plane SDK, Dear ImGui, nlohmann/json"
	@echo "  make build             Build plugin (Release) -> build/xp_wellys_atc.xpl"
	@echo "  make repl              Build headless CLI -> build/atc_repl"
	@echo "  make run-repl          Build + run the CLI (stdin transcripts)"
	@echo "  make test              Run unit tests + scenario tests"
	@echo "  make test-unit         Build + run Catch2 unit tests"
	@echo "  make test-scenarios    Build + run all scenario tests in testscripts/"
	@echo "  make install           Code-sign and install plugin to X-Plane"
	@echo "  make format            Run clang-format on src/*.cpp src/*.hpp"
	@echo "  make lint              Run clang-tidy on src/*.cpp"
	@echo "  make release VERSION=X Tag and push release (writes VERSION.txt)"
	@echo "  make release-build     Build plugin with RELEASE=ON (embeds VERSION.txt)"
	@echo "  make cleanup-tags      Prune local tags no longer on origin"
	@echo "  make cleanup-branches  Prune local branches whose remote is gone"
	@echo "  make cleanup-runs      Delete all GitHub Actions runs except the newest per workflow"
	@echo "  make clean             Remove build/ and build-lint/"
	@echo "  make distclean         clean + remove sdk/ and vendor/ (everything 'make setup' installed)"
	@echo "  make help              Show this help"

# ── Setup ─────────────────────────────────────────────────────────────────────
setup: $(SDK_SENTINEL) $(IMGUI_SENTINEL) $(JSON_SENTINEL) $(CATCH2_SENTINEL)
	@echo "Setup complete. Run 'make build' to compile."

$(SDK_SENTINEL):
	@echo "Downloading X-Plane SDK..."
	@set -euo pipefail; \
	TMP=$$(mktemp -d); \
	trap "rm -rf $$TMP" EXIT; \
	curl -fsSL "https://developer.x-plane.com/wp-content/plugins/code-sample-generation/sdk_zip_files/XPSDK430.zip" \
	     -o "$$TMP/sdk.zip"; \
	unzip -q "$$TMP/sdk.zip" -d "$$TMP/sdk_extracted"; \
	mkdir -p sdk/XPLM sdk/XPWidgets sdk/Libraries/Win sdk/Libraries/Mac; \
	find "$$TMP/sdk_extracted" -path "*/CHeaders/XPLM/*.h"   -exec cp {} sdk/XPLM/ \;; \
	find "$$TMP/sdk_extracted" -path "*/CHeaders/Widgets/*.h" -exec cp {} sdk/XPWidgets/ \;; \
	find "$$TMP/sdk_extracted" -path "*/Libraries/Win/*.lib"  -exec cp {} sdk/Libraries/Win/ \;; \
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
build: $(SDK_SENTINEL) $(IMGUI_SENTINEL) $(JSON_SENTINEL) $(CATCH2_SENTINEL)
	@echo "=== Building xp_wellys_atc ==="
	cmake -B build -DCMAKE_BUILD_TYPE=Release -Wno-dev
	cmake --build build --parallel
	@echo ""
	@file build/xp_wellys_atc.xpl
	@echo "Done. Run 'make install' to deploy."

# ── REPL (headless CLI) ───────────────────────────────────────────────────────
repl: $(SDK_SENTINEL) $(IMGUI_SENTINEL) $(JSON_SENTINEL) $(CATCH2_SENTINEL)
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

test-unit: $(SDK_SENTINEL) $(IMGUI_SENTINEL) $(JSON_SENTINEL) $(CATCH2_SENTINEL)
	@echo "=== Building xp_wellys_atc unit tests ==="
	cmake -B build -DCMAKE_BUILD_TYPE=Release -Wno-dev
	cmake --build build --target xp_wellys_atc_tests --parallel
	@echo ""
	@echo "=== Running unit tests ==="
	@./build/xp_wellys_atc_tests

test-scenarios: repl
	@echo "=== Running scenario tests ==="
	./build/atc_repl run testscripts/*.json

# ── Install ───────────────────────────────────────────────────────────────────
install:
	@if [ ! -f "build/xp_wellys_atc.xpl" ]; then \
	    echo "Plugin not built yet. Run 'make build' first."; exit 1; \
	fi
	@echo "=== Installing xp_wellys_atc ==="
	@mkdir -p "$(PLUGIN_DIR)/mac_x64"
	@cp build/xp_wellys_atc.xpl "$(PLUGIN_DIR)/mac_x64/"
	@xattr -dr com.apple.quarantine "$(PLUGIN_DIR)/mac_x64/xp_wellys_atc.xpl" 2>/dev/null || true
	@codesign --force --deep --sign - "$(PLUGIN_DIR)/mac_x64/xp_wellys_atc.xpl"
	@mkdir -p "$(PLUGIN_DIR)/data"
	@if [ ! -f "$(PLUGIN_DIR)/data/settings.json" ]; then \
	    cp data/settings.json "$(PLUGIN_DIR)/data/"; \
	    echo "Installed: $(PLUGIN_DIR)/data/settings.json"; \
	else \
	    echo "Kept existing settings.json"; \
	fi
	@cp data/atc_prompt_templates.json "$(PLUGIN_DIR)/data/"
	@echo "Installed: $(PLUGIN_DIR)/data/atc_prompt_templates.json"
	@mkdir -p "$(PLUGIN_DIR)/data/regions/eu" "$(PLUGIN_DIR)/data/regions/us"
	@cp data/regions/eu/atc_templates.json "$(PLUGIN_DIR)/data/regions/eu/"
	@cp data/regions/eu/flight_rules.json  "$(PLUGIN_DIR)/data/regions/eu/"
	@cp data/regions/eu/airport_vrps.json  "$(PLUGIN_DIR)/data/regions/eu/"
	@echo "Installed: $(PLUGIN_DIR)/data/regions/eu/*.json"
	@cp data/regions/us/atc_templates.json "$(PLUGIN_DIR)/data/regions/us/"
	@cp data/regions/us/flight_rules.json  "$(PLUGIN_DIR)/data/regions/us/"
	@echo "Installed: $(PLUGIN_DIR)/data/regions/us/*.json"
	@rm -f "$(PLUGIN_DIR)/data/atc_templates.json" \
	       "$(PLUGIN_DIR)/data/flight_rules.json" \
	       "$(PLUGIN_DIR)/data/airport_vrps.json"
	@echo "Installed and signed."

# ── Lint ──────────────────────────────────────────────────────────────────────
format:
	@command -v clang-format >/dev/null 2>&1 || { \
	    echo "clang-format not found. Install with: brew install llvm"; \
	    echo "Then add to PATH: export PATH=\"$$(brew --prefix llvm)/bin:$$PATH\""; \
	    exit 1; }
	clang-format -i src/*.cpp src/*.hpp src/engine/*.cpp src/engine/*.hpp

lint: $(SDK_SENTINEL) $(IMGUI_SENTINEL) $(JSON_SENTINEL) $(CATCH2_SENTINEL)
	@command -v clang-tidy >/dev/null 2>&1 || { \
	    echo "clang-tidy not found. Install with: brew install llvm"; \
	    echo "Then add to PATH: export PATH=\"$$(brew --prefix llvm)/bin:$$PATH\""; \
	    exit 1; }
	cmake -B build-lint -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_OSX_ARCHITECTURES=arm64 -Wno-dev
	clang-tidy -p build-lint --extra-arg="-isysroot" --extra-arg="$(shell xcrun --show-sdk-path)" src/*.cpp src/engine/*.cpp

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

release-build: $(SDK_SENTINEL) $(IMGUI_SENTINEL) $(JSON_SENTINEL) $(CATCH2_SENTINEL)
	@echo "=== Building xp_wellys_atc (release) ==="
	cmake -B build -DCMAKE_BUILD_TYPE=Release -DRELEASE=ON -Wno-dev
	cmake --build build --parallel
	@echo ""
	@file build/xp_wellys_atc.xpl
	@echo "Done. Release build with version from VERSION.txt."

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
	rm -rf build/ build-lint/

# ── Distclean ─────────────────────────────────────────────────────────────────
# Remove everything 'make setup' downloaded so a full re-bootstrap is forced.
distclean: clean
	rm -rf sdk/ vendor/
	@echo "Removed sdk/ and vendor/. Run 'make setup' to re-download dependencies."
