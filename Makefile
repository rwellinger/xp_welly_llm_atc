SHELL := /bin/bash

XPLANE_ROOT := /Users/robertw/X-Plane 12
PLUGIN_DIR  := $(XPLANE_ROOT)/Resources/available plugins/xp_wellys_atc

SDK_SENTINEL   := sdk/XPLM/XPLMPlugin.h
IMGUI_SENTINEL := vendor/imgui/imgui.h
JSON_SENTINEL  := vendor/json.hpp

.PHONY: all setup build install clean format lint

all: build

# ── Setup ─────────────────────────────────────────────────────────────────────
setup: $(SDK_SENTINEL) $(IMGUI_SENTINEL) $(JSON_SENTINEL)
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

# ── Build ─────────────────────────────────────────────────────────────────────
build: $(SDK_SENTINEL) $(IMGUI_SENTINEL) $(JSON_SENTINEL)
	@echo "=== Building xp_wellys_atc ==="
	cmake -B build -DCMAKE_BUILD_TYPE=Release -Wno-dev
	cmake --build build --parallel
	@echo ""
	@file build/xp_wellys_atc.xpl
	@echo "Done. Run 'make install' to deploy."

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
	@echo "Installed and signed."

# ── Lint ──────────────────────────────────────────────────────────────────────
format:
	@command -v clang-format >/dev/null 2>&1 || { \
	    echo "clang-format not found. Install with: brew install llvm"; \
	    echo "Then add to PATH: export PATH=\"$$(brew --prefix llvm)/bin:$$PATH\""; \
	    exit 1; }
	clang-format -i src/*.cpp src/*.hpp

lint: $(SDK_SENTINEL) $(IMGUI_SENTINEL) $(JSON_SENTINEL)
	@command -v clang-tidy >/dev/null 2>&1 || { \
	    echo "clang-tidy not found. Install with: brew install llvm"; \
	    echo "Then add to PATH: export PATH=\"$$(brew --prefix llvm)/bin:$$PATH\""; \
	    exit 1; }
	cmake -B build-lint -DCMAKE_BUILD_TYPE=Release -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DCMAKE_OSX_ARCHITECTURES=arm64 -Wno-dev
	clang-tidy -p build-lint --extra-arg="-isysroot" --extra-arg="$(shell xcrun --show-sdk-path)" src/*.cpp

# ── Clean ─────────────────────────────────────────────────────────────────────
clean:
	rm -rf build/ build-lint/
