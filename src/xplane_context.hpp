#ifndef XPLANE_CONTEXT_HPP
#define XPLANE_CONTEXT_HPP

#include <string>

namespace xplane_context {

struct XPlaneContext {
  double latitude = 0.0;
  double longitude = 0.0;
  float altitude_ft_msl = 0.0f;
  float groundspeed_kts = 0.0f;
  float indicated_airspeed_kts = 0.0f;
  float vertical_speed_fpm = 0.0f;
  float heading_true = 0.0f;
  bool on_ground = true;
  bool engines_running = false;
  float com1_freq_mhz = 0.0f;
  float com2_freq_mhz = 0.0f;
  int active_com = 1;
  std::string aircraft_icao;
  std::string nearest_airport_id;
  bool is_towered_airport = false;
};

void init();
void stop();
void update();

const XPlaneContext &get();

} // namespace xplane_context

#endif // XPLANE_CONTEXT_HPP
