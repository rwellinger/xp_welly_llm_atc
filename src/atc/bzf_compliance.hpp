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
#include <vector>

// BZF-Strict-Mode pilot-utterance conformance checker for the DE/BZF
// profile. Implements NfL Sprechfunk 2024 §25 b) Nr. 1 readback
// obligations: a pilot must read back the safety-relevant items of a
// tower clearance (runway, QNH, frequency, SSR code, etc.). When this
// module reports a missing element, the state machine sends a
// corrective tower response (atc_templates.json :: bzf_strict.*)
// instead of silently absorbing the readback.
//
// SDK-free. Lives in xp_atc_engine OBJECT lib so atc_repl and tests can
// use it directly. Operates on raw (pre-normalize_for_speech) text:
// tower templates contain "QNH {qnh}" / "Piste {runway}", and the
// pilot transcript has already been reverse-normalised by
// de_phraseology::parse_spoken_number(), so both sides speak the same
// digit form when this check runs.
namespace bzf_compliance {

// Safety-relevant readback elements from NfL §25 b) Nr. 1. The enum
// values map 1:1 to keys in atc_templates.json :: bzf_strict (e.g.
// Element::QNH -> "missing_qnh").
enum class Element {
  Callsign,  // §14 c) Nr. 2 — full callsign on all clearances
  Runway,    // §25 b) Nr. 1 ii) — runway operations
  QNH,       // §25 b) Nr. 1 iii) — altimeter setting
  Frequency, // §25 b) Nr. 1 iii) — newly assigned channel
  Squawk,    // §25 b) Nr. 1 iii) — SSR code
};

// Stable lowercase short name ("qnh", "runway", ...) for logging.
const char *element_name(Element e);

// Key into atc_templates.json :: bzf_strict ("missing_qnh", ...).
const char *missing_key(Element e);

// Extract the readback-mandatory elements from a tower response.
// Detects structural markers (the literal token "QNH" + digits, the
// literal "Piste" + digits, a frequency in "NNN.NNN" form, "Squawk"
// + 4 digits). The tower's own callsign use is implicit — every
// readback must repeat the pilot's callsign per §14 c) Nr. 2, so
// Callsign is included whenever the tower response is non-empty.
std::vector<Element> extract_required(const std::string &tower_response);

// Check the pilot's transcript against the required-elements list.
// Returns the subset that is MISSING (empty vector = fully conformant).
// `pilot_callsign` is the phonetic-expanded form from
// settings::pilot_callsign() — substring match is case-insensitive.
std::vector<Element> check_pilot_readback(const std::string &pilot_transcript,
                                          const std::vector<Element> &required,
                                          const std::string &pilot_callsign);

// Build the corrective tower response. With multiple missing elements,
// uses the "missing_multi" template; with exactly one, picks the
// element-specific template. The {callsign} placeholder gets filled
// from the `callsign` argument.
std::string build_correction_response(const std::string &callsign,
                                      const std::vector<Element> &missing);

} // namespace bzf_compliance
