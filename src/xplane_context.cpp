#include "xplane_context.hpp"

#include <XPLMDataAccess.h>
#include <XPLMNavigation.h>

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
static XPLMDataRef dr_barometer = nullptr;
static XPLMDataRef dr_wind_direction = nullptr;
static XPLMDataRef dr_wind_speed = nullptr;

static int frame_counter = 0;

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
  dr_barometer =
      XPLMFindDataRef("sim/weather/barometer_sealevel_inhg");
  dr_wind_direction =
      XPLMFindDataRef("sim/weather/wind_direction_degt");
  dr_wind_speed = XPLMFindDataRef("sim/weather/wind_speed_kt");
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
    ctx.com1_freq_mhz = static_cast<float>(XPLMGetDatai(dr_com1_freq)) / 1000.0f;
  if (dr_com2_freq)
    ctx.com2_freq_mhz = static_cast<float>(XPLMGetDatai(dr_com2_freq)) / 1000.0f;
  if (dr_active_com)
    ctx.active_com = XPLMGetDatai(dr_active_com);

  if (dr_aircraft_icao) {
    char buf[64] = {};
    XPLMGetDatab(dr_aircraft_icao, buf, 0, sizeof(buf) - 1);
    ctx.aircraft_icao = buf;
  }

  if (dr_barometer)
    ctx.qnh_inhg = XPLMGetDataf(dr_barometer);
  if (dr_wind_direction)
    ctx.wind_direction_deg = XPLMGetDataf(dr_wind_direction);
  if (dr_wind_speed)
    ctx.wind_speed_kt = XPLMGetDataf(dr_wind_speed);

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

      XPLMNavRef ils_ref = XPLMFindNavAid(nullptr, icao, &lat, &lon, nullptr,
                                          xplm_Nav_ILS | xplm_Nav_Localizer);
      ctx.is_towered_airport = (ils_ref != XPLM_NAV_NOT_FOUND);
    } else {
      ctx.nearest_airport_id = "";
      ctx.is_towered_airport = false;
    }
  }
}

const XPlaneContext &get() { return ctx; }

} // namespace xplane_context
