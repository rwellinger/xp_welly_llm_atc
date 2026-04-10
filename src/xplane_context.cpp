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

#include "xplane_context.hpp"
#include "settings.hpp"

#include <XPLMDataAccess.h>
#include <XPLMNavigation.h>
#include <XPLMUtilities.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <vector>

namespace xplane_context {

static XPlaneContext ctx;

static XPLMDataRef dr_latitude = nullptr;
static XPLMDataRef dr_longitude = nullptr;
static XPLMDataRef dr_altitude = nullptr;
static XPLMDataRef dr_groundspeed = nullptr;
static XPLMDataRef dr_indicated_airspeed = nullptr;
static XPLMDataRef dr_vertical_speed = nullptr;
static XPLMDataRef dr_heading_true = nullptr;
static XPLMDataRef dr_y_agl = nullptr;
static XPLMDataRef dr_n1_percent = nullptr;
static XPLMDataRef dr_com1_freq = nullptr;
static XPLMDataRef dr_com2_freq = nullptr;
static XPLMDataRef dr_active_com = nullptr;
static XPLMDataRef dr_aircraft_icao = nullptr;
static XPLMDataRef dr_avionics_on = nullptr;
static XPLMDataRef dr_barometer = nullptr;
static XPLMDataRef dr_wind_direction = nullptr;
static XPLMDataRef dr_wind_speed = nullptr;
static XPLMDataRef dr_visibility = nullptr;
static XPLMDataRef dr_cloud_base = nullptr;
static XPLMDataRef dr_cloud_type = nullptr;
static XPLMDataRef dr_temperature = nullptr;
static XPLMDataRef dr_dewpoint = nullptr;

static int frame_counter = 0;

static XPLMDataRef dr_com1_standby = nullptr;
static XPLMDataRef dr_com2_standby = nullptr;

// ── Airport frequency + runway cache (built from apt.dat) ───────
static std::unordered_map<std::string, AirportFrequencies> freq_cache_;
static std::unordered_map<std::string, std::vector<RunwayInfo>> runway_cache_;
static std::unordered_map<std::string, std::string> name_cache_;
static std::atomic<bool> towered_cache_ready_{false};

static constexpr double kDeg2Rad = M_PI / 180.0;
static constexpr double kEarthRadiusM = 6371000.0;

static double haversine_distance(double lat1, double lon1, double lat2,
                                 double lon2) {
  double dlat = (lat2 - lat1) * kDeg2Rad;
  double dlon = (lon2 - lon1) * kDeg2Rad;
  double a = std::sin(dlat / 2) * std::sin(dlat / 2) +
             std::cos(lat1 * kDeg2Rad) * std::cos(lat2 * kDeg2Rad) *
                 std::sin(dlon / 2) * std::sin(dlon / 2);
  return kEarthRadiusM * 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
}

static float initial_bearing(double lat1, double lon1, double lat2,
                              double lon2) {
  double lat1r = lat1 * kDeg2Rad;
  double lat2r = lat2 * kDeg2Rad;
  double dlon = (lon2 - lon1) * kDeg2Rad;
  double y = std::sin(dlon) * std::cos(lat2r);
  double x = std::cos(lat1r) * std::sin(lat2r) -
             std::sin(lat1r) * std::cos(lat2r) * std::cos(dlon);
  double bearing = std::atan2(y, x) / kDeg2Rad;
  return static_cast<float>(std::fmod(bearing + 360.0, 360.0));
}

// ── AirportFrequencies implementation ────────────────────────────

bool AirportFrequencies::has(FrequencyType ft) const {
  for (const auto &f : all)
    if (f.type == ft)
      return true;
  return false;
}

float AirportFrequencies::first_mhz(FrequencyType ft) const {
  for (const auto &f : all)
    if (f.type == ft)
      return static_cast<float>(f.freq_khz) / 1000.0f;
  return 0.0f;
}

FrequencyType AirportFrequencies::lookup(float freq_mhz) const {
  // Convert COM frequency (MHz float) to kHz integer for exact comparison
  auto to_khz = [](float mhz) -> uint32_t {
    return static_cast<uint32_t>(std::round(mhz * 1000.0f));
  };
  uint32_t target = to_khz(freq_mhz);
  // Allow +/- 1 kHz tolerance for float rounding
  for (const auto &f : all) {
    uint32_t diff =
        (target > f.freq_khz) ? target - f.freq_khz : f.freq_khz - target;
    if (diff <= 1)
      return f.type;
  }
  return FrequencyType::UNKNOWN;
}

bool AirportFrequencies::has_ground() const { return has(FrequencyType::GROUND); }

// Surface codes 1 (asphalt) and 2 (concrete) are paved
static bool is_paved(int surface_code) {
  return surface_code == 1 || surface_code == 2;
}

static std::string select_active_runway(const std::vector<RunwayInfo> &runways,
                                        float wind_dir, float wind_speed) {
  if (runways.empty())
    return "";

  // Calm wind (< 3 kt): pick longest paved runway, prefer lower-numbered end
  if (wind_speed < 3.0f) {
    const RunwayInfo *best = nullptr;
    for (const auto &rwy : runways) {
      if (!best || (is_paved(rwy.surface_code) && !is_paved(best->surface_code)) ||
          (is_paved(rwy.surface_code) == is_paved(best->surface_code) &&
           rwy.length_m > best->length_m))
        best = &rwy;
    }
    // Return lower-numbered end
    if (best->end1.number < best->end2.number)
      return best->end1.number;
    return best->end2.number;
  }

  // Wind-based: find runway end with largest headwind component
  // When headwind difference < 1 kt, prefer paved runway
  std::string best_end;
  float best_headwind = -9999.0f;
  float best_length = 0.0f;
  bool best_paved = false;

  for (const auto &rwy : runways) {
    bool paved = is_paved(rwy.surface_code);
    for (const auto *end : {&rwy.end1, &rwy.end2}) {
      float diff =
          std::fmod(wind_dir - end->heading_deg + 540.0f, 360.0f) - 180.0f;
      float headwind = wind_speed * std::cos(diff * static_cast<float>(kDeg2Rad));
      // Clearly better headwind (> 1 kt margin), or within 1 kt margin
      // with paved preference, then headwind, then length as tiebreakers
      bool clearly_better = headwind > best_headwind + 1.0f;
      bool within_margin = headwind > best_headwind - 1.0f;
      bool tiebreak_wins =
          within_margin &&
          (std::make_tuple(paved, headwind, rwy.length_m) >
           std::make_tuple(best_paved, best_headwind, best_length));
      if (clearly_better || tiebreak_wins) {
        best_headwind = headwind;
        best_end = end->number;
        best_length = rwy.length_m;
        best_paved = paved;
      }
    }
  }
  return best_end;
}

static std::string xplane_system_path() {
  char raw[2048] = {};
  XPLMGetSystemPath(raw);
  std::string p(raw);
#if defined(__APPLE__)
  // macOS may return an HFS path (colon-separated, no slashes) — convert
  if (p.find(':') != std::string::npos && p.find('/') == std::string::npos) {
    auto colon = p.find(':');
    std::string posix = p.substr(colon + 1);
    for (char &c : posix)
      if (c == ':')
        c = '/';
    p = "/" + posix;
  }
#endif
  if (!p.empty() && p.back() != '/')
    p += '/';
  return p;
}

// Frequency code mapping: apt.dat row code → FrequencyType
struct FreqCodeMapping {
  int old_code;  // 50-55
  int new_code;  // 1050-1055
  FrequencyType type;
};
static constexpr FreqCodeMapping kFreqCodes[] = {
    {50, 1050, FrequencyType::ATIS},     {51, 1051, FrequencyType::UNICOM},
    {52, 1052, FrequencyType::DELIVERY}, {53, 1053, FrequencyType::GROUND},
    {54, 1054, FrequencyType::TOWER},    {55, 1055, FrequencyType::APPROACH},
};

// Parse a line code from the start of a line. Returns -1 if not a frequency.
static int parse_line_code(const std::string &line) {
  if (line.empty())
    return -1;
  // Try to parse leading digits
  size_t i = 0;
  while (i < line.size() && line[i] >= '0' && line[i] <= '9')
    ++i;
  if (i == 0 || i > 4)
    return -1;
  if (i < line.size() && line[i] != ' ' && line[i] != '\t')
    return -1;
  return std::stoi(line.substr(0, i));
}

static void build_towered_cache() {
  std::string apt_path =
      xplane_system_path() +
      "Global Scenery/Global Airports/Earth nav data/apt.dat";

  std::ifstream file(apt_path);
  if (!file.is_open()) {
    char log[512];
    std::snprintf(log, sizeof(log),
                  "[xp_wellys_atc] WARNING: Could not open %s\n",
                  apt_path.c_str());
    XPLMDebugString(log);
    towered_cache_ready_ = true;
    return;
  }

  XPLMDebugString("[xp_wellys_atc] Building airport cache from apt.dat...\n");

  std::unordered_map<std::string, AirportFrequencies> freqs;
  std::unordered_map<std::string, std::vector<RunwayInfo>> runways;
  std::unordered_map<std::string, std::string> names;
  std::string current_icao;
  std::string line;

  while (std::getline(file, line)) {
    if (line.empty())
      continue;

    int code = parse_line_code(line);

    // Airport header: code 1 (airport), 16 (seaplane), 17 (heliport)
    if (code == 1 || code == 16 || code == 17) {
      // Extract ICAO: 5th whitespace-delimited token
      int token = 0;
      size_t i = 0;
      current_icao.clear();
      while (i < line.size() && token < 5) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
          ++i;
        size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t')
          ++i;
        if (token == 4)
          current_icao = line.substr(start, i - start);
        ++token;
      }
      // Everything after ICAO is the airport name
      if (!current_icao.empty()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t'))
          ++i;
        if (i < line.size()) {
          // Trim trailing whitespace/CR
          size_t end = line.size();
          while (end > i &&
                 (line[end - 1] == ' ' || line[end - 1] == '\t' ||
                  line[end - 1] == '\r'))
            --end;
          names[current_icao] = line.substr(i, end - i);
        }
      }
      continue;
    }

    // Frequency lines: codes 50-55 (old) and 1050-1055 (X-Plane 12)
    for (const auto &fc : kFreqCodes) {
      if (code == fc.old_code || code == fc.new_code) {
        if (current_icao.empty())
          break;
        bool is_new_format = (code >= 1000);
        std::istringstream iss(line);
        std::string code_tok, freq_tok;
        if (iss >> code_tok >> freq_tok) {
          uint32_t freq_int = static_cast<uint32_t>(std::stoul(freq_tok));
          uint32_t freq_khz;
          if (is_new_format) {
            // New format (1050-1055): value is already kHz, e.g. 121900
            freq_khz = freq_int;
          } else {
            // Old format (50-55): value is MHz*100, e.g. 12190 → 121900 kHz
            freq_khz = freq_int * 10;
          }
          freqs[current_icao].all.push_back({freq_khz, fc.type});
        }
        break;
      }
    }

    // Land runway: code 100
    if (code == 100) {
      if (current_icao.empty())
        continue;

      std::istringstream iss(line);
      std::vector<std::string> tokens;
      std::string tok;
      while (iss >> tok)
        tokens.push_back(tok);

      if (tokens.size() < 20)
        continue;

      RunwayInfo rwy;
      rwy.width_m = std::stof(tokens[1]);
      rwy.surface_code = std::stoi(tokens[2]);
      rwy.end1.number = tokens[8];
      rwy.end1.lat = std::stod(tokens[9]);
      rwy.end1.lon = std::stod(tokens[10]);
      rwy.end2.number = tokens[17];
      rwy.end2.lat = std::stod(tokens[18]);
      rwy.end2.lon = std::stod(tokens[19]);

      rwy.length_m = static_cast<float>(haversine_distance(
          rwy.end1.lat, rwy.end1.lon, rwy.end2.lat, rwy.end2.lon));
      rwy.end1.heading_deg = initial_bearing(rwy.end1.lat, rwy.end1.lon,
                                             rwy.end2.lat, rwy.end2.lon);
      rwy.end2.heading_deg =
          std::fmod(rwy.end1.heading_deg + 180.0f, 360.0f);

      runways[current_icao].push_back(rwy);
    }
  }

  freq_cache_ = std::move(freqs);
  runway_cache_ = std::move(runways);
  name_cache_ = std::move(names);
  towered_cache_ready_ = true;

  // Count towered airports for log
  size_t towered_count = 0;
  size_t atis_count = 0;
  for (const auto &kv : freq_cache_) {
    if (kv.second.has(FrequencyType::TOWER))
      ++towered_count;
    if (kv.second.has(FrequencyType::ATIS))
      ++atis_count;
  }

  char log[256];
  std::snprintf(log, sizeof(log),
                "[xp_wellys_atc] Airport cache ready: %zu with freqs (%zu "
                "towered, %zu ATIS), %zu with runway data\n",
                freq_cache_.size(), towered_count, atis_count,
                runway_cache_.size());
  XPLMDebugString(log);
}

const char *frequency_type_name(FrequencyType ft) {
  switch (ft) {
  case FrequencyType::UNKNOWN:
    return "Unknown";
  case FrequencyType::DELIVERY:
    return "Delivery";
  case FrequencyType::GROUND:
    return "Ground";
  case FrequencyType::TOWER:
    return "Tower";
  case FrequencyType::APPROACH:
    return "Approach";
  case FrequencyType::UNICOM:
    return "Unicom";
  case FrequencyType::CTAF:
    return "CTAF";
  case FrequencyType::ATIS:
    return "ATIS";
  }
  return "Unknown";
}

void init() {
  dr_latitude = XPLMFindDataRef("sim/flightmodel/position/latitude");
  dr_longitude = XPLMFindDataRef("sim/flightmodel/position/longitude");
  dr_altitude = XPLMFindDataRef("sim/flightmodel/position/elevation");
  dr_groundspeed = XPLMFindDataRef("sim/flightmodel/position/groundspeed");
  dr_indicated_airspeed =
      XPLMFindDataRef("sim/flightmodel/position/indicated_airspeed");
  dr_vertical_speed = XPLMFindDataRef("sim/flightmodel/position/vh_ind_fpm");
  dr_heading_true = XPLMFindDataRef("sim/flightmodel/position/psi");
  dr_y_agl = XPLMFindDataRef("sim/flightmodel/position/y_agl");
  dr_n1_percent = XPLMFindDataRef("sim/cockpit2/engine/indicators/N1_percent");
  dr_com1_freq =
      XPLMFindDataRef("sim/cockpit2/radios/actuators/com1_frequency_hz_833");
  dr_com2_freq =
      XPLMFindDataRef("sim/cockpit2/radios/actuators/com2_frequency_hz_833");
  dr_active_com =
      XPLMFindDataRef("sim/cockpit2/radios/actuators/audio_com_selection");
  dr_aircraft_icao = XPLMFindDataRef("sim/aircraft/view/acf_ICAO");
  dr_avionics_on = XPLMFindDataRef("sim/cockpit/electrical/avionics_on");
  dr_com1_standby = XPLMFindDataRef(
      "sim/cockpit2/radios/actuators/com1_standby_frequency_hz_833");
  dr_com2_standby = XPLMFindDataRef(
      "sim/cockpit2/radios/actuators/com2_standby_frequency_hz_833");
  // X-Plane 12 "region" weather DataRefs (replacements for deprecated ones)
  dr_barometer =
      XPLMFindDataRef("sim/weather/region/sealevel_pressure_pas"); // Pascals
  dr_wind_direction =
      XPLMFindDataRef("sim/weather/region/wind_direction_degt"); // float[13]
  dr_wind_speed =
      XPLMFindDataRef("sim/weather/region/wind_speed_msc"); // float[13], m/s
  dr_visibility =
      XPLMFindDataRef("sim/weather/region/visibility_reported_sm"); // statute mi
  dr_cloud_base =
      XPLMFindDataRef("sim/weather/region/cloud_base_msl_m"); // float[3]
  dr_cloud_type =
      XPLMFindDataRef("sim/weather/region/cloud_type"); // float[3]
  dr_temperature =
      XPLMFindDataRef("sim/weather/region/sealevel_temperature_c"); // float
  dr_dewpoint =
      XPLMFindDataRef("sim/weather/region/dewpoint_deg_c"); // float[13]

  // Build towered cache on background thread
  std::thread(build_towered_cache).detach();
}

void stop() {
  ctx = XPlaneContext{};
  frame_counter = 0;
}

void update() {
  if (dr_latitude)
    ctx.latitude = XPLMGetDatad(dr_latitude);
  if (dr_longitude)
    ctx.longitude = XPLMGetDatad(dr_longitude);
  if (dr_altitude)
    ctx.altitude_ft_msl =
        static_cast<float>(XPLMGetDatad(dr_altitude) * 3.28084);
  if (dr_groundspeed)
    ctx.groundspeed_kts = XPLMGetDataf(dr_groundspeed) * 1.94384f;
  if (dr_indicated_airspeed)
    ctx.indicated_airspeed_kts = XPLMGetDataf(dr_indicated_airspeed);
  if (dr_vertical_speed)
    ctx.vertical_speed_fpm = XPLMGetDataf(dr_vertical_speed);
  if (dr_heading_true)
    ctx.heading_true = XPLMGetDataf(dr_heading_true);

  if (dr_y_agl) {
    float y_agl = XPLMGetDataf(dr_y_agl);
    ctx.height_agl_ft = y_agl * 3.28084f;
    ctx.on_ground = (y_agl < 0.5f);
  }

  if (dr_n1_percent) {
    float n1[8] = {};
    XPLMGetDatavf(dr_n1_percent, n1, 0, 8);
    ctx.engines_running = (n1[0] > 5.0f);
  }

  if (dr_com1_freq)
    ctx.com1_freq_mhz =
        static_cast<float>(XPLMGetDatai(dr_com1_freq)) / 1000.0f;
  if (dr_com2_freq)
    ctx.com2_freq_mhz =
        static_cast<float>(XPLMGetDatai(dr_com2_freq)) / 1000.0f;
  if (dr_active_com) {
    int raw_com = XPLMGetDatai(dr_active_com);
    // audio_com_selection returns 6=COM1, 7=COM2; normalize to 1/2
    if (raw_com == 6)
      ctx.active_com = 1;
    else if (raw_com == 7)
      ctx.active_com = 2;
    else
      ctx.active_com = raw_com; // pass through if already 1/2
  }

  if (dr_aircraft_icao) {
    char buf[64] = {};
    XPLMGetDatab(dr_aircraft_icao, buf, 0, sizeof(buf) - 1);
    ctx.aircraft_icao = buf;
  }

  if (dr_avionics_on)
    ctx.avionics_on = (XPLMGetDatai(dr_avionics_on) != 0);
  // Barometer: region DataRef returns Pascals, convert to inHg
  if (dr_barometer)
    ctx.qnh_inhg = XPLMGetDataf(dr_barometer) / 3386.39f;
  // Wind: region DataRefs are float[13] arrays, surface layer = index 0
  if (dr_wind_direction) {
    float dir[1] = {};
    XPLMGetDatavf(dr_wind_direction, dir, 0, 1);
    ctx.wind_direction_deg = dir[0];
  }
  if (dr_wind_speed) {
    float spd[1] = {};
    XPLMGetDatavf(dr_wind_speed, spd, 0, 1);
    ctx.wind_speed_kt = std::max(0.0f, spd[0] * 1.94384f); // m/s → kt
  }
  // Visibility: region DataRef returns statute miles, convert to meters
  if (dr_visibility)
    ctx.visibility_m = XPLMGetDataf(dr_visibility) * 1609.34f;
  // Cloud base: float[3] array, lowest layer = index 0
  if (dr_cloud_base) {
    float base_m[1] = {};
    XPLMGetDatavf(dr_cloud_base, base_m, 0, 1);
    ctx.cloud_base_ft_msl = base_m[0] * 3.28084f;
  }
  // Cloud type: float[3] (0=cirrus, 1=stratus, 2=cumulus, 3=cumulonimbus)
  // Map to ATIS categories: 0=clear, 1=few, 2=scattered, 3=broken, 4=overcast
  if (dr_cloud_type) {
    float ct[1] = {};
    XPLMGetDatavf(dr_cloud_type, ct, 0, 1);
    // Use cloud base to determine coverage — if base is very high, treat as
    // clear. The float value indicates cloud type, not coverage directly.
    // We approximate coverage from cloud type + base.
    if (ctx.cloud_base_ft_msl > 50000.0f) {
      ctx.cloud_type = 0; // clear
    } else if (ct[0] < 0.5f) {
      ctx.cloud_type = 1; // cirrus → few
    } else if (ct[0] < 1.5f) {
      ctx.cloud_type = 3; // stratus → broken
    } else if (ct[0] < 2.5f) {
      ctx.cloud_type = 2; // cumulus → scattered
    } else {
      ctx.cloud_type = 4; // cumulonimbus → overcast
    }
  }
  if (dr_temperature)
    ctx.temperature_c = XPLMGetDataf(dr_temperature);
  // Dewpoint: float[13] array, surface layer = index 0
  if (dr_dewpoint) {
    float dp[1] = {};
    XPLMGetDatavf(dr_dewpoint, dp, 0, 1);
    ctx.dewpoint_c = dp[0];
  }

  // Derive frequency type from active COM via airport frequency database
  {
    float active_freq =
        (ctx.active_com == 1) ? ctx.com1_freq_mhz : ctx.com2_freq_mhz;
    ctx.frequency_type = towered_cache_ready_
                             ? ctx.airport_freqs.lookup(active_freq)
                             : FrequencyType::UNKNOWN;
  }

  // Nearest airport lookup — throttled to every 60 frames (~1s)
  if (++frame_counter % 60 == 0) {
    float lat = static_cast<float>(ctx.latitude);
    float lon = static_cast<float>(ctx.longitude);
    XPLMNavRef airport_ref =
        XPLMFindNavAid(nullptr, nullptr, &lat, &lon, nullptr, xplm_Nav_Airport);

    if (airport_ref != XPLM_NAV_NOT_FOUND) {
      char icao[32] = {};
      float apt_lat = 0, apt_lon = 0;
      XPLMGetNavAidInfo(airport_ref, nullptr, &apt_lat, &apt_lon, nullptr,
                        nullptr, nullptr, icao, nullptr, nullptr);
      ctx.nearest_airport_id = icao;
      ctx.airport_lat = apt_lat;
      ctx.airport_lon = apt_lon;

      // Airport name from cache
      auto name_it = name_cache_.find(ctx.nearest_airport_id);
      ctx.nearest_airport_name =
          (name_it != name_cache_.end()) ? name_it->second : "";

      // Lookup in frequency + runway cache (default towered until cache ready)
      if (towered_cache_ready_) {
        auto freq_it = freq_cache_.find(ctx.nearest_airport_id);
        if (freq_it != freq_cache_.end()) {
          ctx.airport_freqs = freq_it->second;
          ctx.is_towered_airport =
              ctx.airport_freqs.has(FrequencyType::TOWER);
          ctx.tower_only =
              ctx.is_towered_airport && !ctx.airport_freqs.has_ground();
          ctx.atis_freq_mhz =
              ctx.airport_freqs.first_mhz(FrequencyType::ATIS);
        } else {
          ctx.airport_freqs = {};
          ctx.is_towered_airport = false;
          ctx.tower_only = false;
          ctx.atis_freq_mhz = 0.0f;
        }

        auto it = runway_cache_.find(ctx.nearest_airport_id);
        if (it != runway_cache_.end()) {
          ctx.runways = it->second;
          ctx.active_runway = select_active_runway(
              ctx.runways, ctx.wind_direction_deg, ctx.wind_speed_kt);
        } else {
          ctx.runways.clear();
          ctx.active_runway.clear();
        }
      } else {
        ctx.is_towered_airport = true;
      }
    } else {
      ctx.nearest_airport_id = "";
      ctx.is_towered_airport = false;
      ctx.tower_only = false;
      ctx.airport_freqs = {};
      ctx.airport_lat = 0.0;
      ctx.airport_lon = 0.0;
      ctx.runways.clear();
      ctx.active_runway.clear();
    }

    // Debug: log frequency status every ~5s
    if (settings::debug_logging() && frame_counter % 300 == 0) {
      float active_freq =
          (ctx.active_com == 1) ? ctx.com1_freq_mhz : ctx.com2_freq_mhz;
      char dbg[256];
      std::snprintf(
          dbg, sizeof(dbg),
          "[xp_wellys_atc][DEBUG] COM%d: %.3f MHz -> %s | Airport: %s "
          "(%zu freqs, ATIS=%.3f, tower_only=%d)\n",
          ctx.active_com, active_freq,
          frequency_type_name(ctx.frequency_type),
          ctx.nearest_airport_id.c_str(), ctx.airport_freqs.all.size(),
          ctx.atis_freq_mhz, ctx.tower_only ? 1 : 0);
      XPLMDebugString(dbg);
    }
  }
}

const XPlaneContext &get() { return ctx; }

void set_standby_freq(uint32_t freq_khz) {
  XPLMDataRef dr =
      (ctx.active_com == 1) ? dr_com1_standby : dr_com2_standby;
  if (dr) {
    XPLMSetDatai(dr, static_cast<int>(freq_khz));
    if (settings::debug_logging()) {
      char dbg[128];
      std::snprintf(dbg, sizeof(dbg),
                    "[xp_wellys_atc][DEBUG] Set COM%d standby to %u "
                    "(%.3f MHz)\n",
                    ctx.active_com, freq_khz,
                    static_cast<float>(freq_khz) / 1000.0f);
      XPLMDebugString(dbg);
    }
  }
}

} // namespace xplane_context
