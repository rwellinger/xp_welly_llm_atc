/*
 * xp_wellys_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#ifndef READBACK_VERIFIER_HPP
#define READBACK_VERIFIER_HPP

#include <string>
#include <vector>

namespace readback_verifier {

struct Mismatch {
    std::string field;       // "runway", "fl", "alt", "freq", "squawk"
    std::string expected;    // canonical expected value
    std::string stated;      // what the pilot said (may be empty)
    // ICAO correction ready to speak, without callsign prefix.
    // e.g. "negative, runway two two, readback"
    std::string correction;
};

// Compare clearance_text (ATC-generated) against readback_text (STT output).
// Normalises NATO phonetics, word-digits, and FL spelling variants before
// comparison. Returns empty when everything checks out or when no verifiable
// values are found in the clearance.
std::vector<Mismatch> check(const std::string &clearance_text,
                            const std::string &readback_text);

// Returns the field names ("runway", "fl", "alt", "freq", "squawk") that are
// present in the clearance AND were correctly stated in this readback turn.
// Used to accumulate verified fields across piecemeal multi-turn readbacks.
std::vector<std::string> matched_fields(const std::string &clearance_text,
                                        const std::string &readback_text);

} // namespace readback_verifier

#endif // READBACK_VERIFIER_HPP
