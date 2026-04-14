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

#ifndef XPLANE_CONTEXT_HPP
#define XPLANE_CONTEXT_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace airspace_db {
struct Controller;
}

namespace xplane_context {

struct RunwayEnd {
  std::string number; // e.g. "09", "27L"
  double lat = 0.0;
  double lon = 0.0;
  float heading_deg = 0.0f; // computed from lat/lon of both ends
};

struct RunwayInfo {
  RunwayEnd end1;
  RunwayEnd end2;
  float width_m = 0.0f;
  float length_m = 0.0f; // computed via haversine
  int surface_code = 0;  // 1=asphalt, 2=concrete, etc.
};

enum class FrequencyType {
  UNKNOWN,
  DELIVERY,
  GROUND,
  TOWER,
  APPROACH,
  UNICOM,
  CTAF,
  ATIS,
};

const char *frequency_type_name(FrequencyType ft);

struct AirportFrequency {
  uint32_t freq_khz = 0; // e.g. 121900 for 121.900 MHz (exact integer, no float)
  FrequencyType type = FrequencyType::UNKNOWN;
};

struct AirportFrequencies {
  std::vector<AirportFrequency> all;

  // Has at least one frequency of the given type?
  bool has(FrequencyType ft) const;
  // First frequency of given type as MHz (0.0f if none)
  float first_mhz(FrequencyType ft) const;
  // Match a COM frequency (MHz) to a FrequencyType (UNKNOWN if no match)
  FrequencyType lookup(float freq_mhz) const;
  // Convenience: has(GROUND)
  bool has_ground() const;
};

struct XPlaneContext {
  double latitude = 0.0;
  double longitude = 0.0;
  float altitude_ft_msl = 0.0f;
  float groundspeed_kts = 0.0f;
  float indicated_airspeed_kts = 0.0f;
  float vertical_speed_fpm = 0.0f;
  float heading_true = 0.0f;
  float height_agl_ft = 0.0f;
  bool on_ground = true;
  bool engines_running = false;
  float com1_freq_mhz = 0.0f;
  float com2_freq_mhz = 0.0f;
  int active_com = 1;
  std::string aircraft_icao;
  std::string nearest_airport_id;   // active airport (may be frequency-tuned)
  std::string geometric_nearest_id; // raw geographic nearest from XPLM
  std::string nearest_airport_name; // from apt.dat, e.g. "Grenchen"
  bool is_towered_airport = false;
  FrequencyType frequency_type = FrequencyType::UNKNOWN;
  bool avionics_on = false;
  float qnh_inhg = 29.92f;
  float wind_direction_deg = 0.0f;
  float wind_speed_kt = 0.0f;
  float visibility_m = 9999.0f;
  float cloud_base_ft_msl = 99999.0f;
  int cloud_type = 0; // 0=clear,1=few,2=scattered,3=broken,4=overcast
  float temperature_c = 15.0f;
  float dewpoint_c = 10.0f;
  float atis_freq_mhz = 0.0f;
  AirportFrequencies airport_freqs; // all frequencies for nearest airport
  bool tower_only = false;          // towered but no separate ground freq
  double airport_lat = 0.0;         // airport position (for range checks)
  double airport_lon = 0.0;
  std::vector<RunwayInfo> runways;
  std::string active_runway; // wind-determined, e.g. "28", "09L"
  // Controllers (TWR/TRACON/CTR from atc.dat) whose polygon + altitude band
  // enclose the current aircraft position. Refreshed once per second.
  // Empty if airspace_db is disabled (atc.dat missing).
  std::vector<const airspace_db::Controller *> enclosing_airspaces;
};

void init();
void stop();
void update();

const XPlaneContext &get();

// Write a frequency (in kHz, e.g. 121900) to the active COM's standby slot
void set_standby_freq(uint32_t freq_khz);

// Tune the active COM directly to a frequency (in kHz). Used when the pilot
// clicks a frequency button in the UI - expected behavior is immediate tune.
void set_active_freq(uint32_t freq_khz);

// ── Airport picker / lock ────────────────────────────────────────
// Force `nearest_airport_id` (and all derived fields) to a specific ICAO.
// Overrides both geometric-nearest and frequency-match logic.
// No-op if ICAO is not in the airport cache.
void lock_airport(const std::string &icao);
void unlock_airport();
const std::string &locked_airport() noexcept;

struct NearbyAirport {
  std::string icao;
  std::string name;
  double distance_nm = 0.0;
  bool has_tower = false;
  bool has_atis = false;
};

// Return up to `max_count` airports within `max_nm` of the aircraft,
// sorted ascending by distance. Empty until the airport cache is ready.
std::vector<NearbyAirport> find_nearby_airports(double max_nm,
                                                size_t max_count);

} // namespace xplane_context

#endif // XPLANE_CONTEXT_HPP
