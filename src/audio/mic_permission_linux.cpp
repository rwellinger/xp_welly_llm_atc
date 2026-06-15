/*
 * xp_wellys_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 */

#include "audio/mic_permission.hpp"

namespace mic_permission {

// On Linux there is no system microphone permission dialog.
bool check_and_request() { return true; }

} // namespace mic_permission
