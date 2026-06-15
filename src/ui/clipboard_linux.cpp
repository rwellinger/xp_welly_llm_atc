/*
 * xp_wellys_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 */

#include "ui/clipboard.hpp"

#include <XPLMUtilities.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace ui::clipboard {

// Reads the system clipboard by shelling out to the appropriate tool:
//   Wayland session ($WAYLAND_DISPLAY set): wl-paste --no-newline
//   X11 session:                            xclip -selection clipboard -o
//
// X-Plane typically runs under XWayland on Wayland desktops (Zorin, Ubuntu),
// but $WAYLAND_DISPLAY is still set in the environment, so wl-paste reaches
// the compositor clipboard, which XWayland bridges. Both paths produce the
// same content in practice.
std::string read_system_text() {
  const char *wayland = std::getenv("WAYLAND_DISPLAY");
  bool use_wayland = (wayland && wayland[0] != '\0');

  static bool probed = false;
  static bool tool_present = false;
  if (!probed) {
    probed = true;
    const char *probe = use_wayland ? "command -v wl-paste >/dev/null 2>&1"
                                    : "command -v xclip >/dev/null 2>&1";
    // NOLINTNEXTLINE(bugprone-command-processor,cert-env33-c)
    tool_present = (::system(probe) == 0);
    if (!tool_present) {
      const char *pkg = use_wayland ? "wl-clipboard" : "xclip";
      char msg[256];
      std::snprintf(msg, sizeof(msg),
                    "[xp_wellys_atc] [Paste] unavailable: %s not found. "
                    "Install with: sudo apt install %s\n",
                    pkg, pkg);
      XPLMDebugString(msg);
    }
  }
  if (!tool_present)
    return {};

  const char *cmd = use_wayland ? "wl-paste --no-newline 2>/dev/null"
                                : "xclip -selection clipboard -o 2>/dev/null";

  // NOLINTNEXTLINE(bugprone-command-processor,cert-env33-c)
  FILE *pipe = ::popen(cmd, "r");
  if (!pipe)
    return {};

  char buf[4096] = {};
  size_t n = std::fread(buf, 1, sizeof(buf) - 1, pipe);
  ::pclose(pipe);

  while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
    --n;

  return std::string(buf, n);
}

} // namespace ui::clipboard
