/*
 * xp_wellys_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once

#include <string>

// BZF-Phraseology-Normalizer: pre-TTS pass that expands numeric aviation
// patterns into ziffernweise spoken form per the German Beschraenkt
// Zugeteiltes Sprechfunkzeugnis convention ("Piste 25" -> "Piste zwo
// fuenf"; "QNH 1013" -> "QNH eins null eins drei Hektopascal").
//
// SDK-free. Lives in xp_atc_engine OBJECT lib so atc_repl and tests can
// use it directly.
//
// Idempotent: triggers only on raw digits, never on already-spelled
// number words. Running normalize_for_speech twice yields the same
// output as running it once.
//
// Region-gated by the caller. atc_session::speak_response() invokes
// this when settings::flow_region() == "DE"; other regions are
// unchanged.
namespace de_phraseology {

std::string normalize_for_speech(const std::string &text);

} // namespace de_phraseology
