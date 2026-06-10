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

#include "core/logging.hpp"

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace cifp_reader {

namespace {

// Per-airport+runway result cache keyed on "ICAO:runway" (e.g. "LFLP:04").
// Avoids re-reading the CIFP .dat file on every flight-loop iteration.
// Populated on first query; cleared on cifp_reader::clear_cache().
static std::unordered_map<std::string, CifpAlt>        g_alt_cache;
static std::unordered_map<std::string, std::string>    g_sid_name_cache;
static std::unordered_map<std::string, CifpBindingAlt> g_binding_alt_cache;
static std::mutex g_alt_cache_mutex;

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

  // Cache lookup — avoids re-reading the file every flight-loop frame.
  std::string cache_key = icao + ":" + active_runway;
  {
    std::lock_guard<std::mutex> lk(g_alt_cache_mutex);
    auto it = g_alt_cache.find(cache_key);
    if (it != g_alt_cache.end())
      return it->second;
  }

  std::string dir = cifp_dir;
  if (dir.back() != '/')
    dir += '/';

  // CIFP files are named by uppercase ICAO, e.g. LFLP.dat
  std::string icao_upper = icao;
  for (char &c : icao_upper)
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

  std::string path = dir + icao_upper + ".dat";
  std::ifstream in(path);
  if (!in.good()) {
    logging::debug("[cifp] file not found: %s (icao=%s rwy=%s) -> fallback",
                   path.c_str(), icao_upper.c_str(), active_runway.c_str());
    return {};
  }

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

  if (best.feet > 0) {
    logging::debug("[cifp] %s rwy %s -> %d ft (is_fl=%d)",
                   icao_upper.c_str(), active_runway.c_str(),
                   best.feet, best.is_fl ? 1 : 0);
    std::lock_guard<std::mutex> lk(g_alt_cache_mutex);
    g_alt_cache[cache_key] = best;
    return best;
  }

  // No SID found for the requested runway.  Try the reciprocal end as a
  // fallback (calm-wind selection may have picked the non-procedure end).
  logging::debug("[cifp] %s rwy %s -> no SID found, trying reciprocal",
                 icao_upper.c_str(), active_runway.c_str());

  // Compute reciprocal: strip optional L/R/C suffix, flip number by ±18.
  if (!active_runway.empty()) {
    std::string digits = active_runway;
    char suffix = 0;
    if (!digits.empty() && (digits.back() == 'L' || digits.back() == 'R' ||
                             digits.back() == 'C')) {
      suffix = digits.back();
      digits.pop_back();
    }
    try {
      int num = std::stoi(digits);
      int recip_num = (num <= 18) ? num + 18 : num - 18;
      char recip_buf[16];
      std::snprintf(recip_buf, sizeof(recip_buf), "%02d", recip_num);
      std::string recip = recip_buf;
      if (suffix == 'L') recip += 'R';
      else if (suffix == 'R') recip += 'L';
      else if (suffix == 'C') recip += 'C';

      // Re-read from the start of the file for the reciprocal runway.
      in.clear();
      in.seekg(0);
      std::string recip_match = "RW" + recip;
      int recip_best_seq = INT_MAX;
      CifpAlt recip_best;

      while (std::getline(in, line)) {
        if (line.size() < 4 || line.compare(0, 4, "SID:") != 0) continue;
        auto f2 = split_csv(line);
        if (f2.size() < 26) continue;
        if (trim(f2[3]) != recip_match) continue;
        std::string seq_str2 = trim(f2[0]);
        if (seq_str2.size() <= 4) continue;
        int seq2 = 0;
        try { seq2 = std::stoi(seq_str2.substr(4)); } catch (...) { continue; }
        std::string pterm2 = trim(f2[11]);
        CifpAlt alt2;
        if (pterm2 == "CF" && f2.size() > 25) alt2 = parse_alt(f2[25]);
        else if ((pterm2 == "DF" || pterm2 == "TF") && f2.size() > 23)
          alt2 = parse_alt(f2[23]);
        if (alt2.feet > 0 && seq2 < recip_best_seq) {
          recip_best_seq = seq2;
          recip_best = alt2;
        }
      }

      if (recip_best.feet > 0) {
        logging::debug("[cifp] %s reciprocal rwy %s -> %d ft (is_fl=%d)",
                       icao_upper.c_str(), recip.c_str(),
                       recip_best.feet, recip_best.is_fl ? 1 : 0);
        std::lock_guard<std::mutex> lk(g_alt_cache_mutex);
        g_alt_cache[cache_key] = recip_best;
        return recip_best;
      }
    } catch (...) {}
  }

  logging::debug("[cifp] %s rwy %s -> fallback (no SID on either end)",
                 icao_upper.c_str(), active_runway.c_str());
  // Cache the negative result too so the file isn't re-read on every frame.
  {
    std::lock_guard<std::mutex> lk(g_alt_cache_mutex);
    g_alt_cache[cache_key] = {};
  }
  return {};
}

// ── Shared CIFP file opener + ICAO upper-caser ──────────────────────────

static std::string make_cifp_path(const std::string &cifp_dir,
                                   const std::string &icao) {
  std::string dir = cifp_dir;
  if (dir.back() != '/') dir += '/';
  std::string upper = icao;
  for (char &c : upper)
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
  return dir + upper + ".dat";
}

// ── sid_name_for_runway ────────────────────────────────────────────────

std::string sid_name_for_runway(const std::string &cifp_dir,
                                 const std::string &icao,
                                 const std::string &active_runway) {
  if (cifp_dir.empty() || icao.empty() || active_runway.empty())
    return {};

  std::string cache_key = icao + ":" + active_runway + ":NAME";
  {
    std::lock_guard<std::mutex> lk(g_alt_cache_mutex);
    auto it = g_sid_name_cache.find(cache_key);
    if (it != g_sid_name_cache.end())
      return it->second;
  }

  std::ifstream in(make_cifp_path(cifp_dir, icao));
  if (!in.good()) {
    std::lock_guard<std::mutex> lk(g_alt_cache_mutex);
    g_sid_name_cache[cache_key] = {};
    return {};
  }

  std::string rwy_match = "RW" + active_runway;
  std::string best_sid;

  std::string line;
  while (std::getline(in, line)) {
    if (line.size() < 4 || line.compare(0, 4, "SID:") != 0) continue;
    auto f = split_csv(line);
    if (f.size() < 5) continue;
    if (trim(f[3]) != rwy_match) continue;
    std::string sid = trim(f[2]);
    if (sid.empty()) continue;
    // Alphabetically first SID is the representative ATC-assigned SID.
    if (best_sid.empty() || sid < best_sid)
      best_sid = sid;
  }

  logging::debug("[cifp] %s rwy %s SID name -> %s",
                 icao.c_str(), active_runway.c_str(),
                 best_sid.empty() ? "(none)" : best_sid.c_str());
  {
    std::lock_guard<std::mutex> lk(g_alt_cache_mutex);
    g_sid_name_cache[cache_key] = best_sid;
  }
  return best_sid;
}

// ── sid_binding_altitude ────────────────────────────────────────────────

CifpBindingAlt sid_binding_altitude(const std::string &cifp_dir,
                                     const std::string &icao,
                                     const std::string &active_runway) {
  if (cifp_dir.empty() || icao.empty() || active_runway.empty())
    return {};

  std::string cache_key = icao + ":" + active_runway + ":BIND";
  {
    std::lock_guard<std::mutex> lk(g_alt_cache_mutex);
    auto it = g_binding_alt_cache.find(cache_key);
    if (it != g_binding_alt_cache.end())
      return it->second;
  }

  std::ifstream in(make_cifp_path(cifp_dir, icao));
  if (!in.good()) {
    std::lock_guard<std::mutex> lk(g_alt_cache_mutex);
    g_binding_alt_cache[cache_key] = {};
    return {};
  }

  std::string rwy_match = "RW" + active_runway;
  CifpBindingAlt best;

  std::string line;
  while (std::getline(in, line)) {
    if (line.size() < 4 || line.compare(0, 4, "SID:") != 0) continue;
    auto f = split_csv(line);
    if (f.size() < 24) continue;
    if (trim(f[3]) != rwy_match) continue;

    std::string pterm = trim(f[11]);
    if (pterm != "DF" && pterm != "TF") continue;  // only leg-end waypoints

    // f[22] = altitude descriptor: '+' = at or above (minimum constraint).
    // ' ' / '' with a non-empty f[23] = "at" altitude (also a hard minimum).
    std::string alt_desc = trim(f[22]);
    bool is_minimum = (alt_desc == "+") || (alt_desc.empty() && !trim(f[23]).empty());
    if (!is_minimum) continue;

    CifpAlt candidate = parse_alt(f[23]);
    if (candidate.feet <= 0) continue;

    if (candidate.feet > best.alt.feet) {
      best.alt      = candidate;
      best.waypoint = trim(f[4]);
      best.sid      = trim(f[2]);
    }
  }

  if (best.alt.feet > 0) {
    logging::debug("[cifp] %s rwy %s binding min -> %d ft (is_fl=%d) at %s (%s)",
                   icao.c_str(), active_runway.c_str(),
                   best.alt.feet, best.alt.is_fl ? 1 : 0,
                   best.waypoint.c_str(), best.sid.c_str());
  }
  {
    std::lock_guard<std::mutex> lk(g_alt_cache_mutex);
    g_binding_alt_cache[cache_key] = best;
  }
  return best;
}

// ────────────────────────────────────────────────────────────────────────

void clear_cache() {
  std::lock_guard<std::mutex> lk(g_alt_cache_mutex);
  g_alt_cache.clear();
  g_sid_name_cache.clear();
  g_binding_alt_cache.clear();
}

std::string preferred_departure_runway(const std::string &cifp_dir,
                                       const std::string &icao) {
  if (cifp_dir.empty() || icao.empty()) return {};

  std::string dir = cifp_dir;
  if (dir.back() != '/') dir += '/';

  std::string icao_upper = icao;
  for (char &c : icao_upper)
    c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));

  std::ifstream in(dir + icao_upper + ".dat");
  if (!in.good()) return {};

  // Count SID records per runway designator, return the one with most entries.
  std::unordered_map<std::string, int> counts;
  std::string line;
  while (std::getline(in, line)) {
    if (line.size() < 4 || line.compare(0, 4, "SID:") != 0) continue;
    auto f = split_csv(line);
    if (f.size() < 4) continue;
    std::string rwy = trim(f[3]);
    if (rwy.size() > 2 && rwy[0] == 'R' && rwy[1] == 'W')
      counts[rwy.substr(2)]++;
  }

  std::string best;
  int best_count = 0;
  for (auto &kv : counts) {
    if (kv.second > best_count) {
      best_count = kv.second;
      best = kv.first;
    }
  }
  return best;
}

} // namespace cifp_reader
