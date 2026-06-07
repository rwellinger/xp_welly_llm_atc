/*
 * xp_wellys_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 */

#include "ui/clipboard.hpp"

#include <cstdio>
#include <cstdlib>
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
    const char *cmd = (wayland && wayland[0] != '\0')
                          ? "wl-paste --no-newline 2>/dev/null"
                          : "xclip -selection clipboard -o 2>/dev/null";

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
