/*
 * xp_wellys_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 *
 * Bridge to the macOS system clipboard. ImGui::GetClipboardText()
 * only reaches the system clipboard when a Platform Backend
 * (imgui_impl_osx.mm, imgui_impl_glfw.cpp, ...) has installed the
 * platform clipboard callbacks. This X-Plane plugin has only the
 * renderer backend (imgui_impl_opengl2) — input + clipboard come
 * from X-Plane's own event system. So ImGui's clipboard reads
 * return only what we (or another ImGui widget) wrote internally,
 * not what the user pasted from outside. Use these helpers
 * instead when we need the actual NSPasteboard contents.
 */

#ifndef UI_CLIPBOARD_HPP
#define UI_CLIPBOARD_HPP

#include <string>

namespace ui::clipboard {

// Returns the current NSPasteboard string contents (UTF-8), or an
// empty string if the pasteboard does not currently hold a string
// value. Never throws.
std::string read_system_text();

} // namespace ui::clipboard

#endif // UI_CLIPBOARD_HPP
