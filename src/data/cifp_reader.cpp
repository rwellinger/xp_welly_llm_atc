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

#include "data/cifp_reader.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace cifp_reader {

namespace {

std::vector<std::string> split_csv(const std::string &line) {
  std::vector<std::string> out;
  std::istringstream ss(line);
  std::string tok;
  while (std::getline(ss, tok, ','))
    out.push_back(tok);
  return out;
}

std::string trim(const std::string &s) {
  const std::string ws = " \t\r\n;";
  size_t start = s.find_first_not_of(ws);
  if (start == std::string::npos) return "";
  size_t end = s.find_last_not_of(ws);
  return s.substr(start, end - start + 1);
}

// Parse an altitude field from the CIFP record.
// "06500"  → {6500, false}  (feet AMSL — express as "6500 feet" in clearance)
// "FL130"  → {13000, true}  (flight level — express as "FL130" in clearance)
// ""       → {0, false}     (field empty, no constraint)
CifpAlt parse_alt(const std::string &raw) {
  std::string s = trim(raw);
  if (s.empty()) return {};

  if (s.size() >= 3 && s[0] == 'F' && s[1] == 'L') {
    try {
      int fl = std::stoi(s.substr(2));
      return {fl * 100, true};
    } catch (...) {}
    return {};
  }

  try {
    int ft = std::stoi(s);
    if (ft > 0) return {ft, false};
  } catch (...) {}
  return {};
}

} // namespace

CifpAlt initial_altitude(const std::string &cifp_dir,
                         const std::string &icao,
                         const std::string &active_runway) {
  if (cifp_dir.empty() || icao.empty() || active_runway.empty())
    return {};

  std::string dir = cifp_dir;
  if (dir.back() != '/')
    dir += '/';

  // CIFP files are named by uppercase ICAO, e.g. LFLP.dat
  std::string icao_upper = icao;
  for (char &c : icao_upper)
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

  std::ifstream in(dir + icao_upper + ".dat");
  if (!in.good()) return {};

  // Runway match string: prepend "RW", e.g. "22" → "RW22", "09L" → "RW09L"
  std::string rwy_match = "RW" + active_runway;

  int best_seq = INT_MAX;
  CifpAlt best;

  std::string line;
  while (std::getline(in, line)) {
    // Only SID records start with "SID:"
    if (line.size() < 4 || line.compare(0, 4, "SID:") != 0)
      continue;

    auto f = split_csv(line);
    if (f.size() < 26) continue;

    // Field[3] = runway designator (e.g. "RW22")
    if (trim(f[3]) != rwy_match) continue;

    // Field[0] = "SID:{seq}", sequence in steps of 10 (010, 020, ...)
    std::string seq_str = trim(f[0]);
    if (seq_str.size() <= 4) continue;
    int seq = 0;
    try { seq = std::stoi(seq_str.substr(4)); } catch (...) { continue; }

    // Field[11] = path terminator
    std::string pterm = trim(f[11]);

    CifpAlt alt;
    if (pterm == "CF" && f.size() > 25) {
      alt = parse_alt(f[25]);
    } else if ((pterm == "DF" || pterm == "TF") && f.size() > 23) {
      alt = parse_alt(f[23]);
    }

    if (alt.feet > 0 && seq < best_seq) {
      best_seq = seq;
      best = alt;
    }
  }

  return best;
}

} // namespace cifp_reader
