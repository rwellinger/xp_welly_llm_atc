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

#include "atc/bzf_compliance.hpp"
#include "atc/atc_templates.hpp"

#include <cctype>
#include <regex>
#include <string>

namespace bzf_compliance {

namespace {

std::string to_lower(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s)
    out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

// Tower-output side: matchers detect that the controller actually
// passed a readback-mandatory item. We match the raw-digit form
// because atc_state_machine::process() emits raw digits in the
// response text; ziffernweise normalisation runs later in
// atc_session::speak_response(). Patterns are deliberately permissive
// — the goal is to discover what the tower asked the pilot to confirm,
// not to validate the controller's own wording.

bool tower_has_qnh(const std::string &lc) {
  // "qnh 1013" / "qnh 998" — case-insensitive, optional separator
  static const std::regex re(R"(\bqnh\s+\d{3,4}\b)");
  return std::regex_search(lc, re);
}

bool tower_has_runway(const std::string &lc) {
  // "piste 25" / "piste 07L" — case-insensitive
  static const std::regex re(R"(\bpiste\s+\d{1,2}[lrc]?\b)");
  return std::regex_search(lc, re);
}

bool tower_has_frequency(const std::string &lc) {
  // Either decimal form (118.300) or hand-off phrase ("frequenz 118")
  static const std::regex re(R"(\b\d{3}\.\d{1,3}\b)");
  if (std::regex_search(lc, re))
    return true;
  static const std::regex re2(R"(\bfrequenz\s+\d)");
  return std::regex_search(lc, re2);
}

bool tower_has_squawk(const std::string &lc) {
  static const std::regex re(R"(\bsquawk\s+\d{4}\b)");
  return std::regex_search(lc, re);
}

// Pilot-input side: same regexes — the reverse-normalizer
// (de_phraseology::parse_spoken_number) has already converted
// "QNH eins null eins drei" back to "QNH 1013" by the time the
// transcript reaches the state machine.

bool pilot_has_qnh(const std::string &lc) { return tower_has_qnh(lc); }
bool pilot_has_runway(const std::string &lc) { return tower_has_runway(lc); }
bool pilot_has_frequency(const std::string &lc) {
  return tower_has_frequency(lc);
}
bool pilot_has_squawk(const std::string &lc) { return tower_has_squawk(lc); }

// Normalise known Whisper NATO-letter mistranscriptions before matching.
// Whisper biases the ATC vocabulary prompt (see atc_prompt_templates.json
// :: whisper_prompt) but the general-language prior still wins on
// acoustically-ambiguous NATO letters — most reliably:
//   "Victor"  → "Vector"   (V — User-confirmed 2026-06-05, HB-DSV test)
//   "Juliett" → "Juliet"   (J — single-T is the general-English variant)
//   "Whiskey" → "Wisky"    (W — vowel-elision in fast speech)
//   "X-Ray"   → "Xray"     (X — dash dropped)
// We normalise both sides bidirectionally to a canonical form so the
// match works regardless of which side has the variant. Operates on
// lowercase input; touches whole-word occurrences via space anchors.
void normalize_whisper_nato_variants(std::string &lc) {
  // (from, to) pairs — `from` is the seen variant, `to` is the canonical
  // NfL form. Bidirectional check below tries `from` AND its NfL form
  // so we don't care which side is the canonical one.
  static const std::pair<const char *, const char *> kVariants[] = {
      {"vector", "victor"},
      {"juliet ", "juliett "},
      {"juliet,", "juliett,"},
      {"wisky", "whiskey"},
      {"whiskee", "whiskey"},
      {"xray", "x-ray"},
      {"x ray", "x-ray"},
  };
  for (const auto &[from, to] : kVariants) {
    std::size_t from_len = std::char_traits<char>::length(from);
    std::size_t to_len = std::char_traits<char>::length(to);
    std::size_t pos = lc.find(from);
    while (pos != std::string::npos) {
      lc.replace(pos, from_len, to);
      pos = lc.find(from, pos + to_len);
    }
  }
  // Trailing-token Juliet without punctuation: handle the end-of-string
  // case the space-anchored entries above miss.
  if (lc.size() >= 7 &&
      lc.compare(lc.size() - 7, 7, " juliet") == 0)
    lc.replace(lc.size() - 7, 7, " juliett");
}

// Substring match — the pilot's phonetic callsign is multi-word
// ("november eins zwo drei alfa bravo"), and Whisper may render only
// a subset. We accept any match of at least the LAST TWO TOKENS of
// the phonetic callsign — that is the BZF-Verkürzung rule from
// NfL §13 b) (Bodenstelle initiates, pilot may follow with first +
// last two characters). Both sides are normalised through
// normalize_whisper_nato_variants() first so common Whisper NATO
// mistranscriptions don't produce false-negative readback flags.
bool pilot_has_callsign(const std::string &pilot_lc,
                        const std::string &callsign_lc) {
  if (callsign_lc.empty())
    return true; // can't check, give the benefit of the doubt
  std::string p = pilot_lc;
  std::string c = callsign_lc;
  normalize_whisper_nato_variants(p);
  normalize_whisper_nato_variants(c);
  // Try full match first.
  if (p.find(c) != std::string::npos)
    return true;
  // Fall back to last two tokens of the callsign (BZF-Verkürzung).
  std::size_t sp1 = c.rfind(' ');
  if (sp1 == std::string::npos)
    return false;
  std::size_t sp2 = c.rfind(' ', sp1 - 1);
  std::string short_form = c.substr(sp2 == std::string::npos ? 0 : sp2 + 1);
  return p.find(short_form) != std::string::npos;
}

} // namespace

const char *element_name(Element e) {
  switch (e) {
  case Element::Callsign:
    return "callsign";
  case Element::Runway:
    return "runway";
  case Element::QNH:
    return "qnh";
  case Element::Frequency:
    return "frequency";
  case Element::Squawk:
    return "squawk";
  }
  return "unknown";
}

const char *missing_key(Element e) {
  switch (e) {
  case Element::Callsign:
    return "missing_callsign";
  case Element::Runway:
    return "missing_runway";
  case Element::QNH:
    return "missing_qnh";
  case Element::Frequency:
    return "missing_frequency";
  case Element::Squawk:
    return "missing_squawk";
  }
  return "missing_multi";
}

std::vector<Element> extract_required(const std::string &tower_response) {
  std::vector<Element> out;
  if (tower_response.empty())
    return out;
  const std::string lc = to_lower(tower_response);
  // Callsign is mandatory on every non-empty clearance per §14 c) Nr. 2.
  out.push_back(Element::Callsign);
  if (tower_has_runway(lc))
    out.push_back(Element::Runway);
  if (tower_has_qnh(lc))
    out.push_back(Element::QNH);
  if (tower_has_frequency(lc))
    out.push_back(Element::Frequency);
  if (tower_has_squawk(lc))
    out.push_back(Element::Squawk);
  return out;
}

std::vector<Element>
check_pilot_readback(const std::string &pilot_transcript,
                     const std::vector<Element> &required,
                     const std::string &pilot_callsign) {
  std::vector<Element> missing;
  const std::string pilot_lc = to_lower(pilot_transcript);
  const std::string cs_lc = to_lower(pilot_callsign);
  for (Element e : required) {
    bool ok = true;
    switch (e) {
    case Element::Callsign:
      ok = pilot_has_callsign(pilot_lc, cs_lc);
      break;
    case Element::Runway:
      ok = pilot_has_runway(pilot_lc);
      break;
    case Element::QNH:
      ok = pilot_has_qnh(pilot_lc);
      break;
    case Element::Frequency:
      ok = pilot_has_frequency(pilot_lc);
      break;
    case Element::Squawk:
      ok = pilot_has_squawk(pilot_lc);
      break;
    }
    if (!ok)
      missing.push_back(e);
  }
  return missing;
}

std::string
build_correction_response(const std::string &callsign,
                          const std::vector<Element> &missing) {
  if (missing.empty())
    return "";
  // Single missing element -> element-specific template.
  // Two or more  -> generic "vollstaendig wiederholen" template.
  const char *tmpl_key =
      (missing.size() == 1) ? missing_key(missing.front()) : "missing_multi";
  // Default text matches the NfL §18 wording in case the JSON block is
  // absent (e.g. EU/US profile with strict mode flipped on by accident).
  std::string tmpl = atc_templates::lookup_bzf_strict(
      tmpl_key, std::string("{callsign}, wiederholen Sie die vollstaendige "
                            "Freigabe."));
  // Substitute {callsign} — keep it simple, no general-purpose render
  // (atc_templates::fill is overkill for one slot).
  const std::string slot = "{callsign}";
  std::size_t pos = tmpl.find(slot);
  while (pos != std::string::npos) {
    tmpl.replace(pos, slot.size(), callsign);
    pos = tmpl.find(slot, pos + callsign.size());
  }
  return tmpl;
}

} // namespace bzf_compliance
