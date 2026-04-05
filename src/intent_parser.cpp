#include "intent_parser.hpp"
#include "settings.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <string>
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

static std::string extract_callsign(const std::string &text) {
  // Look for known phonetic-alphabet sequences (2+ consecutive phonetic words
  // or phonetic words mixed with digits)
  // Also check against pilot_callsign from settings

  std::string pilot_cs = to_lower(settings::pilot_callsign());
  if (!pilot_cs.empty() && contains(text, pilot_cs)) {
    return settings::pilot_callsign();
  }

  // Look for "november" as start of N-number registration
  auto npos = text.find("november");
  if (npos != std::string::npos) {
    // Grab words after "november" that are phonetic or digit words
    std::string after = text.substr(npos);
    std::vector<std::string> words;
    std::string word;
    for (char c : after) {
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

    // Build callsign from consecutive phonetic/digit words
    std::string cs;
    for (const auto &w : words) {
      bool is_phonetic = false;
      for (const auto &pa : kPhoneticAlphabet) {
        if (w == pa) {
          is_phonetic = true;
          break;
        }
      }
      bool is_digit = kSpokenDigits.count(w) > 0;
      if (is_phonetic || is_digit) {
        if (!cs.empty())
          cs += " ";
        // Capitalize first letter
        std::string cw = w;
        cw[0] = static_cast<char>(std::toupper(cw[0]));
        cs += cw;
      } else if (!cs.empty()) {
        break; // end of callsign sequence
      }
    }
    if (!cs.empty())
      return cs;
  }

  // Look for "hotel bravo" or similar 2+ phonetic word sequences
  std::vector<std::string> words;
  std::string word;
  for (char c : text) {
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

  for (size_t i = 0; i < words.size(); ++i) {
    bool is_phonetic = false;
    for (const auto &pa : kPhoneticAlphabet) {
      if (words[i] == pa) {
        is_phonetic = true;
        break;
      }
    }
    if (!is_phonetic)
      continue;

    // Found a phonetic word — collect consecutive phonetic/digit words
    std::string cs;
    size_t j = i;
    int phonetic_count = 0;
    while (j < words.size()) {
      bool jp = false;
      for (const auto &pa : kPhoneticAlphabet) {
        if (words[j] == pa) {
          jp = true;
          break;
        }
      }
      bool jd = kSpokenDigits.count(words[j]) > 0;
      if (!jp && !jd)
        break;
      if (jp)
        ++phonetic_count;
      if (!cs.empty())
        cs += " ";
      std::string cw = words[j];
      cw[0] = static_cast<char>(std::toupper(cw[0]));
      cs += cw;
      ++j;
    }
    // Need at least 2 words to count as callsign
    if (j - i >= 2 && phonetic_count >= 1) {
      // Skip common ATC facility words that happen to be phonetic-like
      if (cs == "Hotel Bravo" || phonetic_count >= 2)
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
  return (contains(t, "clear") && contains(t, "runway")) ||
         contains(t, "vacated");
}

static bool match_request_taxi(const std::string &t) {
  return contains(t, "taxi") || contains(t, "request taxi") ||
         contains(t, "taxiing");
}

static bool match_report_position(const std::string &t) {
  return contains(t, "downwind") || contains(t, "base") ||
         contains(t, "final") || contains(t, "crosswind") ||
         contains(t, "upwind");
}

static bool match_request_landing(const std::string &t) {
  return contains(t, "inbound") || contains(t, "landing") ||
         contains(t, "full stop") || contains(t, "touch and go");
}

static bool match_request_frequency(const std::string &t) {
  return contains(t, "frequency change") || contains(t, "switching") ||
         contains(t, "with you");
}

static bool match_initial_call(const std::string &t) {
  return starts_with(t, "ground") || starts_with(t, "tower") ||
         starts_with(t, "delivery") ||
         // Also match "zurich ground", "kennedy tower", etc.
         contains(t, " ground,") || contains(t, " tower,") ||
         contains(t, " delivery,") || contains(t, " ground ") ||
         contains(t, " tower ") || contains(t, " delivery ");
}

static bool match_readback(const std::string &t) {
  if (contains(t, "wilco") || contains(t, "roger"))
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
    {PilotIntent::SELF_ANNOUNCE, 0.90f, match_self_announce},
    {PilotIntent::READY_FOR_DEPARTURE, 0.90f, match_ready_for_departure},
    {PilotIntent::RUNWAY_VACATED, 0.90f, match_runway_vacated},
    {PilotIntent::REQUEST_TAXI, 0.90f, match_request_taxi},
    {PilotIntent::REPORT_POSITION, 0.90f, match_report_position},
    {PilotIntent::REQUEST_LANDING, 0.85f, match_request_landing},
    {PilotIntent::REQUEST_FREQUENCY, 0.80f, match_request_frequency},
    {PilotIntent::INITIAL_CALL, 0.85f, match_initial_call},
    {PilotIntent::READBACK, 0.75f, match_readback},
};

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

const char *intent_name(PilotIntent intent) {
  switch (intent) {
  case PilotIntent::UNKNOWN:
    return "UNKNOWN";
  case PilotIntent::INITIAL_CALL:
    return "INITIAL_CALL";
  case PilotIntent::REQUEST_TAXI:
    return "REQUEST_TAXI";
  case PilotIntent::READY_FOR_DEPARTURE:
    return "READY_FOR_DEPARTURE";
  case PilotIntent::REPORT_POSITION:
    return "REPORT_POSITION";
  case PilotIntent::REQUEST_LANDING:
    return "REQUEST_LANDING";
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

  // Context weighting adjustments
  if (msg.intent == PilotIntent::SELF_ANNOUNCE && ctx.is_towered_airport) {
    // SELF_ANNOUNCE only valid at non-towered airports
    // At towered airports, reduce confidence significantly
    msg.confidence = 0.3f;
  }

  if (msg.intent == PilotIntent::REQUEST_TAXI && !ctx.on_ground) {
    msg.confidence = 0.3f;
  }

  if (msg.intent == PilotIntent::INITIAL_CALL && !ctx.is_towered_airport &&
      contains(text, "tower")) {
    msg.confidence = 0.4f;
  }

  return msg;
}

} // namespace intent_parser
