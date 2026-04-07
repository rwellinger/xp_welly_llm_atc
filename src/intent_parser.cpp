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

#include "intent_parser.hpp"
#include "settings.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace intent_parser {

void init() {}
void stop() {}

// ---------------------------------------------------------------------------
// Helpers
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
  // Pattern 1: "runway" followed by spoken numbers and optional suffix
  auto pos = text.find("runway");
  if (pos == std::string::npos)
    return {};

  std::string after = text.substr(pos + 6); // skip "runway"
  // trim leading space
  if (!after.empty() && after[0] == ' ')
    after = after.substr(1);

  // Try matching "two six", "one eight", etc. (two single digits)
  std::string runway_num;
  std::string suffix;
  std::string remaining = after;

  // Try compound numbers first ("twenty six", etc.)
  for (auto it = kSpokenDigits.rbegin(); it != kSpokenDigits.rend(); ++it) {
    if (starts_with(remaining, it->first)) {
      runway_num = it->second;
      remaining = remaining.substr(it->first.size());
      if (!remaining.empty() && remaining[0] == ' ')
        remaining = remaining.substr(1);
      break;
    }
  }

  // Try two separate single-digit words ("two six" → "26")
  if (runway_num.empty()) {
    for (const auto &[word1, d1] : kSpokenDigits) {
      if (starts_with(remaining, word1)) {
        std::string rest = remaining.substr(word1.size());
        if (!rest.empty() && rest[0] == ' ')
          rest = rest.substr(1);
        for (const auto &[word2, d2] : kSpokenDigits) {
          if (starts_with(rest, word2)) {
            runway_num = d1 + d2;
            remaining = rest.substr(word2.size());
            if (!remaining.empty() && remaining[0] == ' ')
              remaining = remaining.substr(1);
            break;
          }
        }
        if (!runway_num.empty())
          break;
        // Single digit runway
        runway_num = d1;
        remaining = rest;
        break;
      }
    }
  }

  // Try numeric digits directly ("runway 28")
  if (runway_num.empty()) {
    size_t i = 0;
    while (i < remaining.size() && std::isdigit(remaining[i]))
      ++i;
    if (i > 0) {
      runway_num = remaining.substr(0, i);
      remaining = remaining.substr(i);
      if (!remaining.empty() && remaining[0] == ' ')
        remaining = remaining.substr(1);
    }
  }

  if (runway_num.empty())
    return {};

  // Check for suffix
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

// Strip punctuation (Whisper often outputs "Bravo, Lima, Kilo")
static std::string strip_punctuation(const std::string &s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) {
    if (std::isalpha(static_cast<unsigned char>(c)) || c == ' ')
      out += c;
    else if (std::ispunct(static_cast<unsigned char>(c)))
      out += ' '; // replace punctuation with space
  }
  return out;
}

// Split string into words
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

static bool is_phonetic_word(const std::string &w) {
  for (const auto &pa : kPhoneticAlphabet) {
    if (w == pa)
      return true;
  }
  return false;
}

// Build capitalized callsign from consecutive phonetic/digit words
static std::string collect_phonetic_sequence(
    const std::vector<std::string> &words, size_t start, size_t &end,
    int &phonetic_count) {
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

// Validate extracted callsign against configured pilot callsign.
// Returns true if they share enough phonetic words to be the same callsign.
static bool matches_configured_callsign(const std::string &extracted) {
  std::string pilot_cs = to_lower(settings::pilot_callsign());
  if (pilot_cs.empty())
    return true; // no configured callsign — accept anything

  auto ext_words = split_words(to_lower(extracted));
  auto cfg_words = split_words(pilot_cs);

  // Check if the last 3 words match (abbreviated callsign recognition)
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
  return matches >= static_cast<int>(n) - 1; // allow 1 mismatch for Whisper errors
}

static std::string extract_callsign(const std::string &text) {
  // Strip punctuation before matching (Whisper outputs commas between words)
  std::string clean = strip_punctuation(text);

  // Check for exact match against configured callsign
  std::string pilot_cs = to_lower(settings::pilot_callsign());
  if (!pilot_cs.empty() && contains(clean, pilot_cs)) {
    return settings::pilot_callsign();
  }

  auto words = split_words(clean);

  // Look for "november" as start of N-number registration
  for (size_t i = 0; i < words.size(); ++i) {
    if (words[i] != "november")
      continue;
    size_t end = 0;
    int phonetic_count = 0;
    std::string cs =
        collect_phonetic_sequence(words, i, end, phonetic_count);
    if (!cs.empty() && matches_configured_callsign(cs))
      return cs;
  }

  // Look for 2+ consecutive phonetic/digit word sequences
  for (size_t i = 0; i < words.size(); ++i) {
    if (!is_phonetic_word(words[i]))
      continue;

    size_t end = 0;
    int phonetic_count = 0;
    std::string cs =
        collect_phonetic_sequence(words, i, end, phonetic_count);

    if (end - i >= 2 && phonetic_count >= 1) {
      if ((cs == "Hotel Bravo" || phonetic_count >= 2) &&
          matches_configured_callsign(cs))
        return cs;
    }
  }

  return {};
}

// ---------------------------------------------------------------------------
// Intent detection rules (priority order)
// ---------------------------------------------------------------------------

struct IntentRule {
  PilotIntent intent;
  float confidence;
  bool (*match)(const std::string &text);
};

static bool match_unable(const std::string &t) { return contains(t, "unable"); }

static bool match_self_announce(const std::string &t) {
  return contains(t, "traffic");
}

static bool match_ready_for_departure(const std::string &t) {
  return contains(t, "ready") &&
         (contains(t, "departure") || contains(t, "takeoff") ||
          contains(t, "take off") || contains(t, "holding short"));
}

static bool match_runway_vacated(const std::string &t) {
  // "clear of runway" / "vacated runway" but NOT "cleared for takeoff runway"
  if (contains(t, "vacated"))
    return true;
  if (contains(t, "clear") && contains(t, "runway") &&
      !contains(t, "takeoff") && !contains(t, "take off") &&
      !contains(t, "landing") && !contains(t, "land"))
    return true;
  return false;
}

static bool match_request_taxi_parking(const std::string &t) {
  if (!contains(t, "taxi"))
    return false;
  return contains(t, "parking") || contains(t, "apron") ||
         contains(t, "general aviation") || contains(t, "stand") ||
         contains(t, "gate") || contains(t, "ramp");
}

static bool match_request_taxi(const std::string &t) {
  return contains(t, "taxi") || contains(t, "request taxi") ||
         contains(t, "taxiing");
}

static bool match_radio_check(const std::string &t) {
  return contains(t, "radio check") || contains(t, "how do you read");
}

static bool match_report_position_downwind(const std::string &t) {
  return contains(t, "downwind") && !contains(t, "report downwind") &&
         !contains(t, "takeoff") && !contains(t, "take off");
}

static bool match_report_position_base(const std::string &t) {
  return contains(t, "base") && !contains(t, "base leg to final") &&
         !contains(t, "report base");
}

static bool match_report_position_final(const std::string &t) {
  return contains(t, "final") && !contains(t, "full stop") &&
         !contains(t, "report final");
}

static bool match_report_position(const std::string &t) {
  return contains(t, "crosswind") || contains(t, "upwind");
}

static bool match_request_landing(const std::string &t) {
  return (contains(t, "inbound") || contains(t, "landing") ||
          contains(t, "full stop")) &&
         !contains(t, "touch and go");
}

static bool match_request_touch_and_go(const std::string &t) {
  return contains(t, "touch and go") && !contains(t, "taxi");
}

static bool match_go_around(const std::string &t) {
  return contains(t, "going around") || contains(t, "go around") ||
         contains(t, "missed approach");
}

static bool match_request_frequency(const std::string &t) {
  return contains(t, "frequency change") || contains(t, "switching") ||
         contains(t, "with you");
}

static bool has_facility_keyword(const std::string &t,
                                 const std::string &facility) {
  return starts_with(t, facility) || contains(t, " " + facility + ",") ||
         contains(t, " " + facility + " ") || ends_with(t, " " + facility);
}

static bool match_initial_call_ground(const std::string &t) {
  return has_facility_keyword(t, "ground") ||
         has_facility_keyword(t, "delivery");
}

static bool match_initial_call_inbound(const std::string &t) {
  return has_facility_keyword(t, "tower") &&
         (contains(t, "inbound") || contains(t, "landing") ||
          contains(t, "full stop"));
}

static bool match_initial_call_tower(const std::string &t) {
  return has_facility_keyword(t, "tower");
}

static bool match_initial_call(const std::string &t) {
  return match_initial_call_ground(t) || match_initial_call_tower(t);
}

static bool match_readback(const std::string &t) {
  if (contains(t, "wilco") || contains(t, "roger"))
    return true;
  // Common takeoff/landing readback patterns
  if (contains(t, "cleared") &&
      (contains(t, "takeoff") || contains(t, "take off") ||
       contains(t, "land")))
    return true;
  // Readback of clearance with reporting instruction
  // e.g. "Takeoff runway 06, report downwind" or "Cleared to land, runway 06"
  if ((contains(t, "takeoff") || contains(t, "take off")) &&
      contains(t, "report"))
    return true;
  // Ends with runway identifier pattern (e.g. "two six", "one eight left")
  // Check if transcript ends with a runway-like spoken number
  for (const auto &[word, _] : kRunwaySuffix) {
    if (ends_with(t, word))
      return true;
  }
  // Check for ending with spoken digits
  for (const auto &[word, num] : kSpokenDigits) {
    if (ends_with(t, word)) {
      // Only match if this looks like a runway number (1-36)
      int n = std::stoi(num);
      if (n >= 1 && n <= 36)
        return true;
    }
  }
  return false;
}

// Rules in priority order
static const std::vector<IntentRule> kRules = {
    {PilotIntent::UNABLE, 0.95f, match_unable},
    {PilotIntent::RADIO_CHECK, 0.95f, match_radio_check},
    {PilotIntent::GO_AROUND, 0.95f, match_go_around},
    {PilotIntent::SELF_ANNOUNCE, 0.90f, match_self_announce},
    {PilotIntent::READBACK, 0.90f, match_readback},
    {PilotIntent::READY_FOR_DEPARTURE, 0.90f, match_ready_for_departure},
    {PilotIntent::RUNWAY_VACATED, 0.90f, match_runway_vacated},
    {PilotIntent::REQUEST_TOUCH_AND_GO, 0.90f, match_request_touch_and_go},
    {PilotIntent::REQUEST_TAXI_PARKING, 0.90f, match_request_taxi_parking},
    {PilotIntent::REQUEST_TAXI, 0.90f, match_request_taxi},
    {PilotIntent::REPORT_POSITION_DOWNWIND, 0.90f,
     match_report_position_downwind},
    {PilotIntent::REPORT_POSITION_BASE, 0.90f, match_report_position_base},
    {PilotIntent::REPORT_POSITION_FINAL, 0.90f, match_report_position_final},
    {PilotIntent::REPORT_POSITION, 0.85f, match_report_position},
    {PilotIntent::REQUEST_LANDING, 0.85f, match_request_landing},
    {PilotIntent::REQUEST_FREQUENCY, 0.80f, match_request_frequency},
    {PilotIntent::INITIAL_CALL_GROUND, 0.85f, match_initial_call_ground},
    {PilotIntent::INITIAL_CALL_INBOUND, 0.85f, match_initial_call_inbound},
    {PilotIntent::INITIAL_CALL_TOWER, 0.85f, match_initial_call_tower},
    {PilotIntent::INITIAL_CALL, 0.80f, match_initial_call},
};

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
  case PilotIntent::REQUEST_TAXI:
    return "REQUEST_TAXI";
  case PilotIntent::REQUEST_TAXI_PARKING:
    return "REQUEST_TAXI_PARKING";
  case PilotIntent::READY_FOR_DEPARTURE:
    return "READY_FOR_DEPARTURE";
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
  case PilotIntent::UNABLE:
    return "UNABLE";
  case PilotIntent::SELF_ANNOUNCE:
    return "SELF_ANNOUNCE";
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
      {"REQUEST_TAXI", PilotIntent::REQUEST_TAXI},
      {"REQUEST_TAXI_PARKING", PilotIntent::REQUEST_TAXI_PARKING},
      {"READY_FOR_DEPARTURE", PilotIntent::READY_FOR_DEPARTURE},
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
      {"UNABLE", PilotIntent::UNABLE},
      {"SELF_ANNOUNCE", PilotIntent::SELF_ANNOUNCE},
  };
  auto it = kMap.find(key);
  return it != kMap.end() ? it->second : PilotIntent::UNKNOWN;
}

PilotMessage parse(const std::string &transcript,
                   const xplane_context::XPlaneContext &ctx) {
  PilotMessage msg;
  msg.raw_transcript = transcript;

  std::string text = to_lower(transcript);

  // Extract callsign and runway
  msg.callsign = extract_callsign(text);
  msg.runway = extract_runway(text);

  // Match intent rules in priority order
  for (const auto &rule : kRules) {
    if (rule.match(text)) {
      msg.intent = rule.intent;
      msg.confidence = rule.confidence;
      break;
    }
  }

  // ── Flight-phase intent filter ──────────────────────────────────
  // Validate matched intent against what's physically possible.
  // Ground-only intents while airborne → demote to low confidence.
  // Airborne-only intents while on ground → convert to READBACK
  // (pilot likely reading back an ATC instruction).
  using PI = PilotIntent;

  auto is_ground_only = [](PI i) {
    return i == PI::INITIAL_CALL || i == PI::INITIAL_CALL_GROUND ||
           i == PI::INITIAL_CALL_TOWER || i == PI::REQUEST_TAXI ||
           i == PI::REQUEST_TAXI_PARKING || i == PI::READY_FOR_DEPARTURE ||
           i == PI::RUNWAY_VACATED;
  };

  auto is_airborne_only = [](PI i) {
    return i == PI::REPORT_POSITION || i == PI::REPORT_POSITION_DOWNWIND ||
           i == PI::REPORT_POSITION_BASE || i == PI::REPORT_POSITION_FINAL ||
           i == PI::REQUEST_LANDING || i == PI::REQUEST_TOUCH_AND_GO ||
           i == PI::GO_AROUND;
  };

  if (ctx.on_ground && is_airborne_only(msg.intent)) {
    // Airborne intent while on ground — likely a readback of an
    // instruction (e.g. "takeoff runway 06, report downwind")
    msg.intent = PI::READBACK;
    msg.confidence = 0.85f;
  } else if (!ctx.on_ground && is_ground_only(msg.intent)) {
    // Ground intent while airborne — reduce confidence to trigger GPT
    msg.confidence = 0.3f;
  }

  // ── Airport-type adjustments ──────────────────────────────────
  if (msg.intent == PI::SELF_ANNOUNCE && ctx.is_towered_airport) {
    msg.confidence = 0.3f;
  }

  if ((msg.intent == PI::INITIAL_CALL || msg.intent == PI::INITIAL_CALL_TOWER ||
       msg.intent == PI::INITIAL_CALL_INBOUND) &&
      !ctx.is_towered_airport && contains(text, "tower")) {
    msg.confidence = 0.4f;
  }

  return msg;
}

} // namespace intent_parser
