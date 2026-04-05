#include "xplane_context.hpp"

#include <XPLMDataAccess.h>
#include <XPLMNavigation.h>
#include <XPLMUtilities.h>

#include <atomic>
#include <cstdio>
#include <fstream>
#include <string>
#include <thread>
#include <unordered_set>

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

static int frame_counter = 0;

// ── Towered airport cache (built from apt.dat) ──────────────────
static std::unordered_set<std::string> towered_airports_;
static std::atomic<bool> towered_cache_ready_{false};

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

  XPLMDebugString("[xp_wellys_atc] Building towered airport cache from "
                  "apt.dat...\n");

  std::unordered_set<std::string> towered;
  std::string current_icao;
  std::string line;

  while (std::getline(file, line)) {
    if (line.empty())
      continue;

    // Airport header: "1  <elev> <has_tower> <deprecated> <ICAO> <name...>"
    // Also code 16 (seaplane base) and 17 (heliport)
    if ((line[0] == '1' && (line[1] == ' ' || line[1] == '\t')) ||
        (line.size() > 2 && line[0] == '1' &&
         (line[1] == '6' || line[1] == '7') &&
         (line[2] == ' ' || line[2] == '\t'))) {
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
    }
    // Tower frequency: code 54 (old format) or 1054 (X-Plane 12 format)
    else if (line.size() > 2 &&
             ((line[0] == '5' && line[1] == '4' &&
               (line[2] == ' ' || line[2] == '\t')) ||
              (line.size() > 4 && line[0] == '1' && line[1] == '0' &&
               line[2] == '5' && line[3] == '4' &&
               (line[4] == ' ' || line[4] == '\t')))) {
      if (!current_icao.empty()) {
        towered.insert(current_icao);
      }
    }
  }

  towered_airports_ = std::move(towered);
  towered_cache_ready_ = true;

  char log[128];
  std::snprintf(log, sizeof(log),
                "[xp_wellys_atc] Towered airport cache ready: %zu airports\n",
                towered_airports_.size());
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

static FrequencyType derive_frequency_type(float freq_mhz, bool is_towered) {
  // Common Unicom/CTAF frequencies
  if ((freq_mhz >= 122.775f && freq_mhz <= 122.825f) || // 122.8
      (freq_mhz >= 122.975f && freq_mhz <= 123.025f) || // 123.0
      (freq_mhz >= 122.700f && freq_mhz <= 122.750f) || // 122.725
      (freq_mhz >= 122.900f && freq_mhz <= 122.950f)) { // 122.925
    return is_towered ? FrequencyType::CTAF : FrequencyType::UNICOM;
  }
  // Ground frequencies: 121.6–121.9
  if (freq_mhz >= 121.600f && freq_mhz <= 121.975f) {
    return FrequencyType::GROUND;
  }
  // Delivery: 121.0–121.5 range
  if (freq_mhz >= 121.000f && freq_mhz <= 121.575f) {
    return FrequencyType::DELIVERY;
  }
  // ATIS: 127.0–128.0
  if (freq_mhz >= 127.000f && freq_mhz <= 128.000f) {
    return FrequencyType::ATIS;
  }
  // Tower/Approach: general aviation band
  if (freq_mhz >= 118.000f && freq_mhz <= 136.975f) {
    return is_towered ? FrequencyType::TOWER : FrequencyType::APPROACH;
  }
  return FrequencyType::UNKNOWN;
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
  dr_barometer = XPLMFindDataRef("sim/weather/barometer_sealevel_inhg");
  dr_wind_direction = XPLMFindDataRef("sim/weather/wind_direction_degt");
  dr_wind_speed = XPLMFindDataRef("sim/weather/wind_speed_kt");

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
    ctx.on_ground = (y_agl < 0.5f) && (ctx.groundspeed_kts < 5.0f);
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
  if (dr_active_com)
    ctx.active_com = XPLMGetDatai(dr_active_com);

  if (dr_aircraft_icao) {
    char buf[64] = {};
    XPLMGetDatab(dr_aircraft_icao, buf, 0, sizeof(buf) - 1);
    ctx.aircraft_icao = buf;
  }

  if (dr_avionics_on)
    ctx.avionics_on = (XPLMGetDatai(dr_avionics_on) != 0);
  if (dr_barometer)
    ctx.qnh_inhg = XPLMGetDataf(dr_barometer);
  if (dr_wind_direction)
    ctx.wind_direction_deg = XPLMGetDataf(dr_wind_direction);
  if (dr_wind_speed)
    ctx.wind_speed_kt = XPLMGetDataf(dr_wind_speed);

  // Derive frequency type from active COM
  {
    float active_freq =
        (ctx.active_com == 1) ? ctx.com1_freq_mhz : ctx.com2_freq_mhz;
    ctx.frequency_type =
        derive_frequency_type(active_freq, ctx.is_towered_airport);
  }

  // Nearest airport lookup — throttled to every 60 frames (~1s)
  if (++frame_counter % 60 == 0) {
    float lat = static_cast<float>(ctx.latitude);
    float lon = static_cast<float>(ctx.longitude);
    XPLMNavRef airport_ref =
        XPLMFindNavAid(nullptr, nullptr, &lat, &lon, nullptr, xplm_Nav_Airport);

    if (airport_ref != XPLM_NAV_NOT_FOUND) {
      char icao[32] = {};
      XPLMGetNavAidInfo(airport_ref, nullptr, nullptr, nullptr, nullptr,
                        nullptr, nullptr, icao, nullptr, nullptr);
      ctx.nearest_airport_id = icao;

      // Lookup in towered cache (default true until cache is ready)
      if (towered_cache_ready_) {
        ctx.is_towered_airport =
            towered_airports_.count(ctx.nearest_airport_id) > 0;
      } else {
        ctx.is_towered_airport = true;
      }
    } else {
      ctx.nearest_airport_id = "";
      ctx.is_towered_airport = false;
    }
  }
}

const XPlaneContext &get() { return ctx; }

} // namespace xplane_context
