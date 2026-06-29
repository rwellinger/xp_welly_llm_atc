/*
 * xp_wellys_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "atc/readback_verifier.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <string>

namespace readback_verifier {

// ── Normalisation helpers ─────────────────────────────────────────────────

static std::string to_lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), ::tolower);
  return s;
}

// Replace all whole-word occurrences of `from` with `to` in `s`.
static void replace_word(std::string &s, const std::string &from,
                         const std::string &to) {
  std::string::size_type pos = 0;
  while ((pos = s.find(from, pos)) != std::string::npos) {
    bool left  = (pos == 0 || !std::isalpha(static_cast<unsigned char>(s[pos - 1])));
    bool right = (pos + from.size() >= s.size() ||
                  !std::isalpha(static_cast<unsigned char>(s[pos + from.size()])));
    if (left && right) {
      s.replace(pos, from.size(), to);
      pos += to.size();
    } else {
      pos += 1;
    }
  }
}

// Convert NATO phonetic digits and "decimal"/"point" to ASCII equivalents.
// "niner zero" → "9 0",  "decimal" → ".",  "fife" → "5", etc.
static std::string normalize_phonetics(const std::string &raw) {
  std::string s = to_lower(raw);
  // Order matters: longer words first to avoid partial matches.
  static const std::pair<const char *, const char *> kMap[] = {
      {"niner",   "9"}, {"seven",   "7"}, {"eight",   "8"},
      {"three",   "3"}, {"decimal", "."}, {"point",   "."},
      {"zero",    "0"}, {"one",     "1"}, {"two",     "2"},
      {"four",    "4"}, {"fife",    "5"}, {"five",    "5"},
      {"six",     "6"}, {"nine",    "9"},
      {"to",      "2"}, // STT mishears "two" as "to" (e.g. "118.200" → "118 decimal to 00")
  };
  for (const auto &[from, to] : kMap)
    replace_word(s, from, to);
  return s;
}

// Compact isolated single digits separated by a space: "9 0" → "90",
// "2 2" → "22", "1 6 6 1" → "1661".  Applied repeatedly until stable.
// Word-boundary anchors ensure we don't collapse digits inside multi-digit
// tokens (e.g. "FL 90" → only the "9 0" part compacts, not "FL90").
static std::string compact_digit_spaces(const std::string &s) {
  static const std::regex kRe(R"((\b\d) (\d\b))");
  std::string prev;
  std::string cur = s;
  do {
    prev = cur;
    cur  = std::regex_replace(cur, kRe, "$1$2");
  } while (cur != prev);
  return cur;
}

// Full normalisation pipeline for a readback transcript.
static std::string normalise(const std::string &text) {
  return compact_digit_spaces(normalize_phonetics(text));
}

// ── Value extractors ──────────────────────────────────────────────────────

// Returns runway number as int (e.g. 22) or -1 if absent.
static int extract_runway(const std::string &norm) {
  static const std::regex kRe(R"(\brunway\s*(\d{1,2})[lLrRcC]?\b)",
                               std::regex_constants::icase);
  std::smatch m;
  if (std::regex_search(norm, m, kRe))
    return std::stoi(m[1]);
  return -1;
}

// Returns flight level as int (90 for FL90 / "flight level 90" / "FL 090")
// or 0 if absent.
static int extract_fl(const std::string &norm) {
  static const std::regex kRe(
      R"((?:fl\s*|flight\s+level\s+)(\d{1,3})\b)",
      std::regex_constants::icase);
  std::smatch m;
  if (std::regex_search(norm, m, kRe))
    return std::stoi(m[1]);  // stoi("090") == 90
  return 0;
}

// Returns altitude in feet (e.g. 6500) or 0 if absent.
static int extract_alt_ft(const std::string &norm) {
  static const std::regex kRe(R"((\d{3,5})\s*(?:feet|ft)\b)",
                               std::regex_constants::icase);
  std::smatch m;
  if (std::regex_search(norm, m, kRe))
    return std::stoi(m[1]);
  return 0;
}

// Returns frequency as "NNN.NNN" string or "" if absent.
// Handles "121.205", "121 decimal 205" (after phonetics), and spaced
// digit triplets "1 2 1 . 2 0 5".
static std::string extract_freq(const std::string &norm) {
  // Standard format after normalisation: "121.205"
  static const std::regex kStd(R"(\b(\d{3})\.(\d{3})\b)");
  std::smatch m;
  if (std::regex_search(norm, m, kStd))
    return m[1].str() + "." + m[2].str();

  // Spaced variant: digits may have spaces — "1 2 1.2 0 5"
  static const std::regex kSpaced(
      R"((\d[\s]?\d[\s]?\d)\s*[.,]\s*(\d[\s]?\d[\s]?\d))");
  if (std::regex_search(norm, m, kSpaced)) {
    std::string p1 = m[1].str();
    std::string p2 = m[2].str();
    p1.erase(std::remove(p1.begin(), p1.end(), ' '), p1.end());
    p2.erase(std::remove(p2.begin(), p2.end(), ' '), p2.end());
    if (p1.size() == 3 && p2.size() == 3)
      return p1 + "." + p2;
  }
  return {};
}

// Returns squawk code as 4-char string or "" if absent.
static std::string extract_squawk(const std::string &norm) {
  static const std::regex kRe(R"(\bsquawk\s*(\d{4})\b)",
                               std::regex_constants::icase);
  std::smatch m;
  if (std::regex_search(norm, m, kRe))
    return m[1].str();
  return {};
}

// ── Formatting helpers ────────────────────────────────────────────────────

// Format a runway number for ICAO speech ("22" → "two two", "09" → "zero niner").
static std::string runway_to_speech(int rwy) {
  static const char *kDigit[] = {
      "zero", "one", "two", "three", "four",
      "five", "six", "seven", "eight", "niner",
  };
  char buf[4];
  std::snprintf(buf, sizeof(buf), "%02d", rwy);
  std::string out;
  for (char c : std::string(buf)) {
    if (!out.empty()) out += ' ';
    out += kDigit[c - '0'];
  }
  return out;  // "two two", "zero niner"
}

// Format a flight level for ICAO speech (90 → "niner zero", 120 → "one two zero").
static std::string fl_to_speech(int fl) {
  static const char *kDigit[] = {
      "zero", "one", "two", "three", "four",
      "five", "six", "seven", "eight", "niner",
  };
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%d", fl);
  // Pad to at least 2 digits for single-digit levels (unlikely but safe)
  std::string s(buf);
  std::string out;
  for (char c : s) {
    if (!out.empty()) out += ' ';
    out += kDigit[c - '0'];
  }
  return out;  // 90 → "niner zero", 120 → "one two zero"
}

// ── Public API ────────────────────────────────────────────────────────────

std::vector<Mismatch> check(const std::string &clearance_text,
                            const std::string &readback_text) {
  std::vector<Mismatch> out;
  if (clearance_text.empty() || readback_text.empty())
    return out;

  // Normalise both texts: clearance is clean ATC text; readback is raw STT.
  const std::string cl = normalise(clearance_text);
  const std::string rb = normalise(readback_text);

  // ── Runway ─────────────────────────────────────────────────────────────
  int cl_rwy = extract_runway(cl);
  if (cl_rwy >= 0) {
    int rb_rwy = extract_runway(rb);
    if (rb_rwy < 0 || rb_rwy != cl_rwy) {
      Mismatch m;
      m.field    = "runway";
      m.expected = std::to_string(cl_rwy);
      m.stated   = rb_rwy >= 0 ? std::to_string(rb_rwy) : "";
      m.correction = "negative, runway " + runway_to_speech(cl_rwy) +
                     ", readback";
      out.push_back(std::move(m));
    }
  }

  // ── Flight level ───────────────────────────────────────────────────────
  int cl_fl = extract_fl(cl);
  if (cl_fl > 0) {
    int rb_fl = extract_fl(rb);
    if (rb_fl <= 0 || rb_fl != cl_fl) {
      Mismatch m;
      m.field    = "fl";
      m.expected = std::to_string(cl_fl);
      m.stated   = rb_fl > 0 ? std::to_string(rb_fl) : "";
      m.correction = "negative, flight level " + fl_to_speech(cl_fl) +
                     ", readback";
      out.push_back(std::move(m));
    }
  }

  // ── Altitude in feet (when no FL assigned) ─────────────────────────────
  if (cl_fl == 0) {
    int cl_alt = extract_alt_ft(cl);
    if (cl_alt > 0) {
      int rb_alt = extract_alt_ft(rb);
      // Allow ±100 ft tolerance for minor STT digit transpositions.
      if (rb_alt <= 0 || std::abs(rb_alt - cl_alt) > 100) {
        Mismatch m;
        m.field    = "alt";
        m.expected = std::to_string(cl_alt);
        m.stated   = rb_alt > 0 ? std::to_string(rb_alt) : "";
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%d feet", cl_alt);
        m.correction = std::string("negative, ") + buf + ", readback";
        out.push_back(std::move(m));
      }
    }
  }

  // ── Frequency ──────────────────────────────────────────────────────────
  std::string cl_freq = extract_freq(cl);
  if (!cl_freq.empty()) {
    std::string rb_freq = extract_freq(rb);
    if (rb_freq.empty() || rb_freq != cl_freq) {
      Mismatch m;
      m.field      = "freq";
      m.expected   = cl_freq;
      m.stated     = rb_freq;
      m.correction = "negative, " + cl_freq + ", readback";
      out.push_back(std::move(m));
    }
  }

  // ── Squawk ─────────────────────────────────────────────────────────────
  std::string cl_sq = extract_squawk(cl);
  if (!cl_sq.empty()) {
    std::string rb_sq = extract_squawk(rb);
    if (rb_sq.empty() || rb_sq != cl_sq) {
      Mismatch m;
      m.field      = "squawk";
      m.expected   = cl_sq;
      m.stated     = rb_sq;
      m.correction = "negative, squawk " + cl_sq + ", readback";
      out.push_back(std::move(m));
    }
  }

  return out;
}

std::vector<std::string> matched_fields(const std::string &clearance_text,
                                        const std::string &readback_text) {
  std::vector<std::string> ok;
  if (clearance_text.empty() || readback_text.empty())
    return ok;

  const std::string cl = normalise(clearance_text);
  const std::string rb = normalise(readback_text);

  int cl_rwy = extract_runway(cl);
  if (cl_rwy >= 0 && extract_runway(rb) == cl_rwy)
    ok.push_back("runway");

  int cl_fl = extract_fl(cl);
  if (cl_fl > 0 && extract_fl(rb) == cl_fl)
    ok.push_back("fl");

  if (cl_fl == 0) {
    int cl_alt = extract_alt_ft(cl);
    if (cl_alt > 0) {
      int rb_alt = extract_alt_ft(rb);
      if (rb_alt > 0 && std::abs(rb_alt - cl_alt) <= 100)
        ok.push_back("alt");
    }
  }

  std::string cl_freq = extract_freq(cl);
  if (!cl_freq.empty() && extract_freq(rb) == cl_freq)
    ok.push_back("freq");

  std::string cl_sq = extract_squawk(cl);
  if (!cl_sq.empty() && extract_squawk(rb) == cl_sq)
    ok.push_back("squawk");

  return ok;
}

} // namespace readback_verifier
