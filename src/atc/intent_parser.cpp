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

#include "atc/intent_parser.hpp"
#include "atc/intent_rules.hpp"
#include "core/logging.hpp"
#include "data/airport_vrps.hpp"
#include "persistence/settings.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace intent_parser {

// Lazy-init guard so the rule table is loaded on first parse() call. Lets
// tests and the headless atc_repl use parse() without requiring an
// explicit module init() in their stubs.
static std::atomic<bool> g_rules_loaded{false};

void init() {
  intent_rules::init();
  g_rules_loaded = intent_rules::is_loaded();
}

void stop() {
  intent_rules::stop();
  g_rules_loaded = false;
}

// ---------------------------------------------------------------------------
// String helpers (private — feature extractors share these)
// ---------------------------------------------------------------------------

static std::string to_lower(const std::string &s) {
  std::string out = s;
  std::transform(out.begin(), out.end(), out.begin(),
                 [](unsigned char c) { return std::tolower(c); });
  return out;
}

static bool contains(const std::string &hay, const std::string &needle) {
  return hay.find(needle) != std::string::npos;
}

static bool starts_with(const std::string &hay, const std::string &needle) {
  return hay.rfind(needle, 0) == 0;
}

static bool ends_with(const std::string &hay, const std::string &needle) {
  if (needle.size() > hay.size())
    return false;
  return hay.compare(hay.size() - needle.size(), needle.size(), needle) == 0;
}

// ---------------------------------------------------------------------------
// Spoken-number → digit mapping for runway extraction
// ---------------------------------------------------------------------------

static const std::map<std::string, std::string> kSpokenDigits = {
    {"zero", "0"},          {"one", "1"},           {"two", "2"},
    {"three", "3"},         {"four", "4"},          {"five", "5"},
    {"six", "6"},           {"seven", "7"},         {"eight", "8"},
    {"nine", "9"},          {"niner", "9"},         {"ten", "10"},
    {"eleven", "11"},       {"twelve", "12"},       {"thirteen", "13"},
    {"fourteen", "14"},     {"fifteen", "15"},      {"sixteen", "16"},
    {"seventeen", "17"},    {"eighteen", "18"},     {"nineteen", "19"},
    {"twenty", "20"},       {"twenty one", "21"},   {"twenty two", "22"},
    {"twenty three", "23"}, {"twenty four", "24"},  {"twenty five", "25"},
    {"twenty six", "26"},   {"twenty seven", "27"}, {"twenty eight", "28"},
    {"twenty nine", "29"},  {"thirty", "30"},       {"thirty one", "31"},
    {"thirty two", "32"},   {"thirty three", "33"}, {"thirty four", "34"},
    {"thirty five", "35"},  {"thirty six", "36"},
};

static const std::map<std::string, std::string> kRunwaySuffix = {
    {"left", "L"},
    {"right", "R"},
    {"center", "C"},
};

static std::string extract_runway(const std::string &text) {
  // Find anchor word "runway".
  std::size_t pos = text.find("runway");
  std::size_t anchor_len = 6;
  if (pos == std::string::npos)
    return {};

  std::string after = text.substr(pos + anchor_len);
  if (!after.empty() && after[0] == ' ')
    after = after.substr(1);

  std::string runway_num;
  std::string suffix;
  std::string remaining = after;

  // Try compound numbers first ("twenty six", "twenty two", etc.) — EN only.
  // Skip single-digit entries (value.size() == 1) so "two two" is not
  // short-circuited to "2" — those fall through to the two-word path below.
  for (auto it = kSpokenDigits.rbegin(); it != kSpokenDigits.rend(); ++it) {
    if (it->second.size() == 1)
      continue;
    if (starts_with(remaining, it->first)) {
      runway_num = it->second;
      remaining = remaining.substr(it->first.size());
      if (!remaining.empty() && remaining[0] == ' ')
        remaining = remaining.substr(1);
      break;
    }
  }

  // Try two separate single-digit words ("two six" -> "26").
  auto try_single_digit = [&](const std::map<std::string, std::string> &m,
                              const std::string &input, std::string &out_digit,
                              std::string &out_remaining) -> bool {
    for (const auto &[word, digit] : m) {
      if (starts_with(input, word)) {
        out_digit = digit;
        out_remaining = input.substr(word.size());
        if (!out_remaining.empty() && out_remaining[0] == ' ')
          out_remaining = out_remaining.substr(1);
        return true;
      }
    }
    return false;
  };

  if (runway_num.empty()) {
    std::string d1;
    std::string rest;
    bool got1 = try_single_digit(kSpokenDigits, remaining, d1, rest);
    if (got1) {
      std::string d2;
      std::string rest2;
      bool got2 = try_single_digit(kSpokenDigits, rest, d2, rest2);
      if (got2) {
        runway_num = d1 + d2;
        remaining = rest2;
      } else {
        runway_num = d1;
        remaining = rest;
      }
    }
  }

  // Try numeric digits directly ("runway 28"). Whisper occasionally renders
  // two-digit runways with a hyphen or space separator ("1-4", "1 4");
  // collect up to two digits across one such separator. Stops after 2 digits
  // because no real runway number exceeds 36.
  if (runway_num.empty()) {
    size_t i = 0;
    std::string digits;
    while (i < remaining.size() && digits.size() < 2) {
      if (std::isdigit(static_cast<unsigned char>(remaining[i]))) {
        digits += remaining[i];
        ++i;
      } else if (!digits.empty() &&
                 (remaining[i] == '-' || remaining[i] == ' ') &&
                 i + 1 < remaining.size() &&
                 std::isdigit(static_cast<unsigned char>(remaining[i + 1]))) {
        ++i; // skip single hyphen or space between digits
      } else {
        break;
      }
    }
    if (!digits.empty()) {
      runway_num = digits;
      remaining = remaining.substr(i);
      if (!remaining.empty() && remaining[0] == ' ')
        remaining = remaining.substr(1);
    }
  }

  if (runway_num.empty())
    return {};

  // Check for suffix (L/R/C).
  for (const auto &[word, code] : kRunwaySuffix) {
    if (starts_with(remaining, word)) {
      suffix = code;
      break;
    }
  }

  return runway_num + suffix;
}

// ---------------------------------------------------------------------------
// Callsign extraction
// ---------------------------------------------------------------------------

static const std::vector<std::string> kPhoneticAlphabet = {
    "alpha",  "bravo",   "charlie", "delta",  "echo",   "foxtrot", "golf",
    "hotel",  "india",   "juliet",  "kilo",   "lima",   "mike",    "november",
    "oscar",  "papa",    "quebec",  "romeo",  "sierra", "tango",   "uniform",
    "victor", "whiskey", "xray",    "yankee", "zulu",
};

// Voxtral STT commonly mishears NATO phonetic alphabet words.
// Applied word-by-word before intent scoring and before storing the transcript
// so the corrected word appears both in the ATC display and callsign matching.
// "rome" is included even though it could be a navaid name: in practice the
// pilot is almost always spelling a callsign suffix, and the navaid context
// ("direct Rome") still parses correctly after the replacement.
// Word-level STT alias corrections (one misheard word → one canonical word).
static const std::unordered_map<std::string, std::string> kWordAliases = {
    // Romeo (R) — Voxtral mishearings:
    {"rainbow", "romeo"},
    {"railway", "romeo"},
    {"rumble", "romeo"},
    {"romo", "romeo"},
    {"rome", "romeo"},
    // Common ATC word mishearings:
    {"content", "contact"}, // "content tower" → "contact tower"
};

// Phrase-level corrections (multi-word → different word count).
// Note: phrase pass runs AFTER the word-alias pass, so use post-word-alias
// forms (e.g. "romeo mayrou" not "rome mayrou" — "rome" is already → "romeo" by
// then).
static const std::vector<std::pair<std::string, std::string>> kPhraseAliases = {
    {"stopped up", "startup"}, // "stopped up approved" → "startup approved"
    {"chamber area",
     "chambery"},              // "chamber area approach" → "chambery approach"
    {"romeo mayrou", "romeo"}, // "rome, mayrou" (Voxtral split) → "romeo"
    {"can be", "climbing to"}, // "can be 6500 feet" → "climbing to 6500 feet"
};

// Strip punctuation (Whisper often outputs "Bravo, Lima, Kilo")
static std::string strip_punctuation(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (std::isalpha(static_cast<unsigned char>(c)) || c == ' ')
      out += c;
    else if (std::ispunct(static_cast<unsigned char>(c)))
      out += ' ';
  }
  return out;
}

static std::vector<std::string> split_words(const std::string &s) {
  std::vector<std::string> words;
  std::string word;
  for (char c : s) {
    if (c == ' ') {
      if (!word.empty())
        words.push_back(word);
      word.clear();
    } else {
      word += c;
    }
  }
  if (!word.empty())
    words.push_back(word);
  return words;
}

static std::string apply_phonetic_aliases(const std::string &s) {
  // Word-level pass.
  auto words = split_words(s);
  for (auto &w : words) {
    auto it = kWordAliases.find(w);
    if (it != kWordAliases.end())
      w = it->second;
  }
  std::string out;
  out.reserve(s.size());
  for (const auto &w : words) {
    if (!out.empty())
      out += ' ';
    out += w;
  }
  // Phrase-level pass (multi-word substitutions).
  for (const auto &[from, to] : kPhraseAliases) {
    size_t pos = 0;
    while ((pos = out.find(from, pos)) != std::string::npos) {
      out.replace(pos, from.size(), to);
      pos += to.size();
    }
  }
  return out;
}

static bool is_phonetic_word(const std::string &w) {
  for (const auto &pa : kPhoneticAlphabet) {
    if (w == pa)
      return true;
  }
  return false;
}

static std::string
collect_phonetic_sequence(const std::vector<std::string> &words, size_t start,
                          size_t &end, int &phonetic_count) {
  std::string cs;
  phonetic_count = 0;
  end = start;
  while (end < words.size()) {
    bool jp = is_phonetic_word(words[end]);
    bool jd = kSpokenDigits.count(words[end]) > 0;
    if (!jp && !jd)
      break;
    if (jp)
      ++phonetic_count;
    if (!cs.empty())
      cs += " ";
    std::string cw = words[end];
    cw[0] = static_cast<char>(std::toupper(cw[0]));
    cs += cw;
    ++end;
  }
  return cs;
}

static bool matches_configured_callsign(const std::string &extracted) {
  std::string pilot_cs = to_lower(settings::pilot_callsign());
  if (pilot_cs.empty())
    return true;

  auto ext_words = split_words(to_lower(extracted));
  auto cfg_words = split_words(pilot_cs);

  size_t n = std::min({ext_words.size(), cfg_words.size(), size_t(3)});
  if (n == 0)
    return false;

  size_t ext_off = ext_words.size() - n;
  size_t cfg_off = cfg_words.size() - n;
  int matches = 0;
  for (size_t i = 0; i < n; ++i) {
    if (ext_words[ext_off + i] == cfg_words[cfg_off + i])
      ++matches;
  }
  return matches >= static_cast<int>(n) - 1;
}

static std::string extract_callsign(const std::string &text) {
  std::string clean = strip_punctuation(text);

  std::string pilot_cs = to_lower(settings::pilot_callsign());
  if (!pilot_cs.empty() && contains(clean, pilot_cs)) {
    return settings::pilot_callsign();
  }

  auto words = split_words(clean);

  // Anchor trigger: "november" is the prefix letter for N-number callsigns.
  for (size_t i = 0; i < words.size(); ++i) {
    bool is_trigger = words[i] == "november";
    if (!is_trigger)
      continue;
    size_t end = 0;
    int phonetic_count = 0;
    std::string cs = collect_phonetic_sequence(words, i, end, phonetic_count);
    if (!cs.empty() && matches_configured_callsign(cs))
      return cs;
  }

  for (size_t i = 0; i < words.size(); ++i) {
    if (!is_phonetic_word(words[i]))
      continue;

    size_t end = 0;
    int phonetic_count = 0;
    std::string cs = collect_phonetic_sequence(words, i, end, phonetic_count);

    if (end - i >= 2 && phonetic_count >= 1) {
      if ((cs == "Hotel Bravo" || phonetic_count >= 2) &&
          matches_configured_callsign(cs))
        return cs;
    }
  }

  return {};
}

// ---------------------------------------------------------------------------
// Position-keyword detection (drives msg.has_position)
// ---------------------------------------------------------------------------

static bool detect_has_position(const std::string &text) {
  static const std::vector<std::string> markers = {
      // EN — apron / parking / taxi positions
      "on parking",
      "at parking",
      "from parking",
      "on the apron",
      "on apron",
      "on the ramp",
      "on ramp",
      "at stand",
      "at gate",
      "near the hangar",
      "near the tower",
      "on taxiway",
      "south apron",
      "north apron",
      "east apron",
      "west apron",
      "south side",
      "north side",
      "parking position",
      "at the parking",
      "general aviation parking",
      "hangar",
  };
  for (const auto &m : markers)
    if (contains(text, m))
      return true;
  return false;
}

// ---------------------------------------------------------------------------
// ICAO self-correction phraseology
// ---------------------------------------------------------------------------

// If the pilot said "correction" mid-transmission, everything after the last
// "correction" replaces the original content.  Example: "request taxi runway
// 28 correction runway 16" -> parse only "runway 16".
//
// Exception: when the prefix BEFORE "correction" is empty or just a negation
// marker ("no", "negative"), this is NEGATIVE_CORRECTION phraseology, not
// self-correction.  Example: "No correction, request VFR departure" -- the
// pilot is rejecting ATC's last clearance.
static std::string strip_self_correction(std::string text) {
  auto corr_pos = text.rfind("correction");
  if (corr_pos == std::string::npos)
    return text;

  std::string prefix = text.substr(0, corr_pos);
  while (!prefix.empty() && (prefix.back() == ' ' || prefix.back() == ',' ||
                             prefix.back() == '.' || prefix.back() == '\t'))
    prefix.pop_back();
  bool prefix_is_negation = prefix.empty() || prefix == "no" ||
                            prefix == "negative" || ends_with(prefix, " no") ||
                            ends_with(prefix, " negative");

  size_t start = corr_pos + std::string("correction").size();
  while (start < text.size() &&
         (text[start] == ',' || text[start] == ' ' || text[start] == '.'))
    ++start;
  if (start < text.size() && !prefix_is_negation) {
    std::string stripped = text.substr(start);
    if (settings::debug_logging())
      logging::debug("Correction detected, re-parsing: \"%s\"",
                     stripped.c_str());
    return stripped;
  }
  return text;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

const char *intent_name(PilotIntent intent) {
  switch (intent) {
  case PilotIntent::UNKNOWN:
    return "UNKNOWN";
  case PilotIntent::RADIO_CHECK:
    return "RADIO_CHECK";
  case PilotIntent::INITIAL_CALL:
    return "INITIAL_CALL";
  case PilotIntent::INITIAL_CALL_GROUND:
    return "INITIAL_CALL_GROUND";
  case PilotIntent::INITIAL_CALL_TOWER:
    return "INITIAL_CALL_TOWER";
  case PilotIntent::INITIAL_CALL_INBOUND:
    return "INITIAL_CALL_INBOUND";
  case PilotIntent::INITIAL_CALL_INBOUND_VRP:
    return "INITIAL_CALL_INBOUND_VRP";
  case PilotIntent::INITIAL_CALL_APPROACH:
    return "INITIAL_CALL_APPROACH";
  case PilotIntent::REQUEST_TAXI:
    return "REQUEST_TAXI";
  case PilotIntent::REQUEST_TAXI_PARKING:
    return "REQUEST_TAXI_PARKING";
  case PilotIntent::READY_FOR_DEPARTURE:
    return "READY_FOR_DEPARTURE";
  case PilotIntent::READY_FOR_DEPARTURE_VFR:
    return "READY_FOR_DEPARTURE_VFR";
  case PilotIntent::REPORT_POSITION:
    return "REPORT_POSITION";
  case PilotIntent::REPORT_POSITION_DOWNWIND:
    return "REPORT_POSITION_DOWNWIND";
  case PilotIntent::REPORT_POSITION_BASE:
    return "REPORT_POSITION_BASE";
  case PilotIntent::REPORT_POSITION_FINAL:
    return "REPORT_POSITION_FINAL";
  case PilotIntent::REQUEST_LANDING:
    return "REQUEST_LANDING";
  case PilotIntent::REQUEST_TOUCH_AND_GO:
    return "REQUEST_TOUCH_AND_GO";
  case PilotIntent::GO_AROUND:
    return "GO_AROUND";
  case PilotIntent::RUNWAY_VACATED:
    return "RUNWAY_VACATED";
  case PilotIntent::READBACK:
    return "READBACK";
  case PilotIntent::REQUEST_FREQUENCY:
    return "REQUEST_FREQUENCY";
  case PilotIntent::LEAVING_FREQUENCY:
    return "LEAVING_FREQUENCY";
  case PilotIntent::UNABLE:
    return "UNABLE";
  case PilotIntent::SELF_ANNOUNCE:
    return "SELF_ANNOUNCE";
  case PilotIntent::REQUEST_FLIGHT_FOLLOWING:
    return "REQUEST_FLIGHT_FOLLOWING";
  case PilotIntent::INAPPROPRIATE_LANGUAGE:
    return "INAPPROPRIATE_LANGUAGE";
  case PilotIntent::NEGATIVE_CORRECTION:
    return "NEGATIVE_CORRECTION";
  case PilotIntent::TRAFFIC_IN_SIGHT:
    return "TRAFFIC_IN_SIGHT";
  case PilotIntent::TRAFFIC_NEGATIVE_CONTACT:
    return "TRAFFIC_NEGATIVE_CONTACT";
  case PilotIntent::TRAFFIC_LOOKING:
    return "TRAFFIC_LOOKING";
  case PilotIntent::REQUEST_REPEAT:
    return "REQUEST_REPEAT";
  case PilotIntent::REQUEST_IFR_CLEARANCE:
    return "REQUEST_IFR_CLEARANCE";
  case PilotIntent::REQUEST_STARTUP:
    return "REQUEST_STARTUP";
  case PilotIntent::REPORT_HOLDING_SHORT:
    return "REPORT_HOLDING_SHORT";
  case PilotIntent::INITIAL_CALL_CENTER:
    return "INITIAL_CALL_CENTER";
  }
  return "UNKNOWN";
}

const char *intent_template_key(PilotIntent intent) {
  switch (intent) {
  case PilotIntent::INITIAL_CALL:
    return "INITIAL_CALL_TOWER"; // default fallback for generic initial call
  case PilotIntent::REPORT_POSITION:
    return "REPORT_POSITION";
  default:
    return intent_name(intent);
  }
}

PilotIntent intent_from_key(const std::string &key) {
  static const std::unordered_map<std::string, PilotIntent> kMap = {
      {"RADIO_CHECK", PilotIntent::RADIO_CHECK},
      {"INITIAL_CALL", PilotIntent::INITIAL_CALL},
      {"INITIAL_CALL_GROUND", PilotIntent::INITIAL_CALL_GROUND},
      {"INITIAL_CALL_TOWER", PilotIntent::INITIAL_CALL_TOWER},
      {"INITIAL_CALL_INBOUND", PilotIntent::INITIAL_CALL_INBOUND},
      {"INITIAL_CALL_INBOUND_VRP", PilotIntent::INITIAL_CALL_INBOUND_VRP},
      {"INITIAL_CALL_APPROACH", PilotIntent::INITIAL_CALL_APPROACH},
      {"REQUEST_TAXI", PilotIntent::REQUEST_TAXI},
      {"REQUEST_TAXI_PARKING", PilotIntent::REQUEST_TAXI_PARKING},
      {"READY_FOR_DEPARTURE", PilotIntent::READY_FOR_DEPARTURE},
      {"READY_FOR_DEPARTURE_VFR", PilotIntent::READY_FOR_DEPARTURE_VFR},
      {"REPORT_POSITION", PilotIntent::REPORT_POSITION},
      {"REPORT_POSITION_DOWNWIND", PilotIntent::REPORT_POSITION_DOWNWIND},
      {"REPORT_POSITION_BASE", PilotIntent::REPORT_POSITION_BASE},
      {"REPORT_POSITION_FINAL", PilotIntent::REPORT_POSITION_FINAL},
      {"REQUEST_LANDING", PilotIntent::REQUEST_LANDING},
      {"REQUEST_TOUCH_AND_GO", PilotIntent::REQUEST_TOUCH_AND_GO},
      {"GO_AROUND", PilotIntent::GO_AROUND},
      {"RUNWAY_VACATED", PilotIntent::RUNWAY_VACATED},
      {"READBACK", PilotIntent::READBACK},
      {"_READBACK", PilotIntent::READBACK},
      {"REQUEST_FREQUENCY", PilotIntent::REQUEST_FREQUENCY},
      {"LEAVING_FREQUENCY", PilotIntent::LEAVING_FREQUENCY},
      {"UNABLE", PilotIntent::UNABLE},
      {"SELF_ANNOUNCE", PilotIntent::SELF_ANNOUNCE},
      {"REQUEST_FLIGHT_FOLLOWING", PilotIntent::REQUEST_FLIGHT_FOLLOWING},
      {"INAPPROPRIATE_LANGUAGE", PilotIntent::INAPPROPRIATE_LANGUAGE},
      {"NEGATIVE_CORRECTION", PilotIntent::NEGATIVE_CORRECTION},
      {"TRAFFIC_IN_SIGHT", PilotIntent::TRAFFIC_IN_SIGHT},
      {"TRAFFIC_NEGATIVE_CONTACT", PilotIntent::TRAFFIC_NEGATIVE_CONTACT},
      {"TRAFFIC_LOOKING", PilotIntent::TRAFFIC_LOOKING},
      {"REQUEST_REPEAT", PilotIntent::REQUEST_REPEAT},
      {"REQUEST_IFR_CLEARANCE", PilotIntent::REQUEST_IFR_CLEARANCE},
      {"REQUEST_STARTUP", PilotIntent::REQUEST_STARTUP},
      {"REPORT_HOLDING_SHORT", PilotIntent::REPORT_HOLDING_SHORT},
      {"INITIAL_CALL_CENTER", PilotIntent::INITIAL_CALL_CENTER},
  };
  auto it = kMap.find(key);
  return it != kMap.end() ? it->second : PilotIntent::UNKNOWN;
}

PilotMessage parse(const std::string &transcript,
                   const xplane_context::XPlaneContext &ctx) {
  // Lazy load — keeps tests + atc_repl simple (no need for explicit init()
  // wiring in their stubs).
  if (!g_rules_loaded.load(std::memory_order_acquire)) {
    intent_rules::init();
    g_rules_loaded.store(intent_rules::is_loaded(), std::memory_order_release);
  }

  PilotMessage msg;

  // 1. Lowercase + strip ICAO self-correction prefix + phonetic alias fix
  std::string text =
      apply_phonetic_aliases(strip_self_correction(to_lower(transcript)));
  // Store the alias-corrected text so the transcript display shows "Romeo"
  // rather than "Rainbow" / "Railway" / etc.
  msg.raw_transcript = text;

  // 2. Apply Whisper-normalize from JSON (currently empty in EU/US — the
  //    individual rules already have explicit "take of"/"clear for" patterns,
  //    so normalization stays a no-op until we want a global rewrite layer)
  text = intent_rules::preprocess(text);

  // 3. Feature extraction (callsign / runway / VRP / position marker)
  msg.callsign = extract_callsign(text);
  msg.runway = extract_runway(text);
  msg.vrp_name = airport_vrps::find_in_transcript(ctx.nearest_airport_id, text);
  msg.has_position = detect_has_position(text);

  // 4. Match intent against the data-driven rule table
  auto m = intent_rules::match(text);
  msg.intent = m.intent;
  msg.confidence = m.confidence;

  // 5. Apply post-match adjustments (VRP upgrade, phase filter, airport-type
  //    mismatch demotions). Order is the JSON `adjustments` array order; each
  //    adjustment sees the current msg state, so chained rules compose.
  intent_rules::apply_adjustments(msg, ctx, text);

  return msg;
}

} // namespace intent_parser
