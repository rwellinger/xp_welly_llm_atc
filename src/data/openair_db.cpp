/*
 * xp_wellys_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 */

#include "data/openair_db.hpp"
#include "core/logging.hpp"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace openair_db {

namespace {

struct Entry {
  std::string   name;
  AirspaceClass ac_class   = AirspaceClass::OTHER;
  int           floor_ft   = 0;
  int           ceiling_ft = 0;
  // Bounding box for fast rejection.
  double bbox_min_lat = 0.0, bbox_max_lat = 0.0;
  double bbox_min_lon = 0.0, bbox_max_lon = 0.0;
  double bbox_area    = 0.0; // (lat span) * (lon span) — proxy for polygon size
  std::vector<std::pair<double, double>> polygon; // (lat, lon) pairs
};

// Point-in-polygon via ray casting.
static bool point_in_polygon(double lat, double lon,
                              const std::vector<std::pair<double, double>> &poly) {
  if (poly.size() < 3) return false;
  bool inside = false;
  std::size_t n = poly.size();
  for (std::size_t i = 0, j = n - 1; i < n; j = i++) {
    double xi = poly[i].second, yi = poly[i].first;
    double xj = poly[j].second, yj = poly[j].first;
    if (((yi > lat) != (yj > lat)) &&
        (lon < (xj - xi) * (lat - yi) / (yj - yi) + xi))
      inside = !inside;
  }
  return inside;
}

// Parse "DD:MM:SS N DDD:MM:SS E" or "DD:MM:SS S DDD:MM:SS W".
static bool parse_dp(const char *s, double &lat, double &lon) {
  int latd, latm, lond, lonm;
  double lats, lons;
  char latdir[4] = {}, londir[4] = {};
  if (std::sscanf(s, " %d:%d:%lf %3s %d:%d:%lf %3s",
                  &latd, &latm, &lats, latdir,
                  &lond, &lonm, &lons, londir) != 8)
    return false;
  lat = latd + latm / 60.0 + lats / 3600.0;
  if (latdir[0] == 'S') lat = -lat;
  lon = lond + lonm / 60.0 + lons / 3600.0;
  if (londir[0] == 'W') lon = -lon;
  return true;
}

// Parse "AH/AL <value>" — supports "FL095", "4000 MSL", "2500 AGL",
// "GND", "SFC", "UNLIM", plain integers.
// Note: "AGL" values are stored as-is (MSL not known at parse time);
// callers that need MSL must add airport elevation themselves.
static int parse_alt(const char *val) {
  while (*val == ' ') ++val;
  if (std::strncmp(val, "FL", 2) == 0)
    return static_cast<int>(std::strtol(val + 2, nullptr, 10)) * 100;
  if (std::strncmp(val, "GND", 3) == 0 || std::strncmp(val, "SFC", 3) == 0)
    return 0;
  if (std::strncmp(val, "UNLIM", 5) == 0)
    return 99999;
  return static_cast<int>(std::strtol(val, nullptr, 10));
}

static AirspaceClass parse_class(const char *s) {
  if (std::strcmp(s, "CTR") == 0) return AirspaceClass::CTR;
  if (std::strcmp(s, "TMA") == 0) return AirspaceClass::TMA;
  if (std::strcmp(s, "CTA") == 0) return AirspaceClass::CTA;
  if (std::strcmp(s, "FIR") == 0) return AirspaceClass::FIR;
  if (std::strcmp(s, "UIR") == 0) return AirspaceClass::UIR;
  return AirspaceClass::OTHER;
}

// Only index these classes — skip restricted/prohibited/danger areas.
static bool is_indexed(AirspaceClass c) {
  return c == AirspaceClass::CTR || c == AirspaceClass::TMA ||
         c == AirspaceClass::CTA || c == AirspaceClass::FIR ||
         c == AirspaceClass::UIR;
}

std::vector<Entry> s_entries;
std::atomic<bool>  s_ready{false};

static void load(const std::string &path) {
  FILE *f = std::fopen(path.c_str(), "r");
  if (!f) {
    logging::info("openair_db: file not found (%s)", path.c_str());
    s_ready = true;
    return;
  }

  std::vector<Entry> entries;
  bool  active = false;
  Entry cur;

  char line[512];
  while (std::fgets(line, sizeof(line), f)) {
    std::size_t len = std::strlen(line);
    while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r' ||
                       line[len-1] == ' '))
      line[--len] = '\0';
    if (len == 0 || line[0] == '*') continue;

    if (std::strncmp(line, "AC ", 3) == 0) {
      if (active && cur.polygon.size() >= 3) {
        cur.bbox_area = (cur.bbox_max_lat - cur.bbox_min_lat) *
                        (cur.bbox_max_lon - cur.bbox_min_lon);
        entries.push_back(std::move(cur));
      }
      cur    = Entry{};
      cur.ac_class = parse_class(line + 3);
      active = is_indexed(cur.ac_class);
      continue;
    }
    if (!active) continue;

    if (std::strncmp(line, "AN ", 3) == 0) {
      cur.name = line + 3;
    } else if (std::strncmp(line, "AH ", 3) == 0) {
      cur.ceiling_ft = parse_alt(line + 3);
    } else if (std::strncmp(line, "AL ", 3) == 0) {
      cur.floor_ft = parse_alt(line + 3);
    } else if (std::strncmp(line, "DP ", 3) == 0) {
      double lat, lon;
      if (parse_dp(line + 3, lat, lon)) {
        if (cur.polygon.empty()) {
          cur.bbox_min_lat = cur.bbox_max_lat = lat;
          cur.bbox_min_lon = cur.bbox_max_lon = lon;
        } else {
          if (lat < cur.bbox_min_lat) cur.bbox_min_lat = lat;
          if (lat > cur.bbox_max_lat) cur.bbox_max_lat = lat;
          if (lon < cur.bbox_min_lon) cur.bbox_min_lon = lon;
          if (lon > cur.bbox_max_lon) cur.bbox_max_lon = lon;
        }
        cur.polygon.emplace_back(lat, lon);
      }
    }
    // DA / DB arc records skipped — surrounding DP points approximate arcs.
  }
  if (active && cur.polygon.size() >= 3) {
    cur.bbox_area = (cur.bbox_max_lat - cur.bbox_min_lat) *
                    (cur.bbox_max_lon - cur.bbox_min_lon);
    entries.push_back(std::move(cur));
  }

  std::fclose(f);
  s_entries = std::move(entries);
  logging::info("openair_db: loaded %zu airspace entries from %s",
                s_entries.size(), path.c_str());
  s_ready = true;
}

std::thread s_thread;

} // namespace

void init(std::string path) {
  if (path.empty()) { s_ready = true; return; }
  s_thread = std::thread([p = std::move(path)]() { load(p); });
}

void stop() {
  if (s_thread.joinable()) s_thread.join();
  s_entries.clear();
  s_ready = false;
}

bool ready() { return s_ready.load(); }

AirspaceEntry find_enclosing(double lat, double lon, int alt_ft) {
  if (!s_ready) return {};
  const Entry *best = nullptr;
  for (const auto &e : s_entries) {
    // Bounding-box fast reject.
    if (lat < e.bbox_min_lat || lat > e.bbox_max_lat) continue;
    if (lon < e.bbox_min_lon || lon > e.bbox_max_lon) continue;
    // Altitude range check.
    if (alt_ft < e.floor_ft)   continue;
    if (e.ceiling_ft > 0 && alt_ft > e.ceiling_ft) continue;
    // Full polygon check.
    if (!point_in_polygon(lat, lon, e.polygon)) continue;
    // Pick the innermost (smallest bbox area).
    if (!best || e.bbox_area < best->bbox_area)
      best = &e;
  }
  if (!best) return {};
  return {best->name, best->ac_class, best->floor_ft, best->ceiling_ft};
}

int ctr_ceiling_ft(double lat, double lon) {
  if (!s_ready) return 0;
  for (const auto &e : s_entries) {
    if (e.ac_class != AirspaceClass::CTR) continue;
    if (lat < e.bbox_min_lat || lat > e.bbox_max_lat) continue;
    if (lon < e.bbox_min_lon || lon > e.bbox_max_lon) continue;
    if (point_in_polygon(lat, lon, e.polygon))
      return e.ceiling_ft;
  }
  return 0;
}

} // namespace openair_db
