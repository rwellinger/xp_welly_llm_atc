#include "xplane_context.hpp"

#include <XPLMDataAccess.h>

namespace xplane_context {

static XPlaneContext ctx;

static XPLMDataRef dr_latitude          = nullptr;
static XPLMDataRef dr_longitude         = nullptr;
static XPLMDataRef dr_altitude          = nullptr;
static XPLMDataRef dr_groundspeed       = nullptr;
static XPLMDataRef dr_indicated_airspeed = nullptr;
static XPLMDataRef dr_vertical_speed    = nullptr;
static XPLMDataRef dr_heading_true      = nullptr;
static XPLMDataRef dr_on_ground         = nullptr;
static XPLMDataRef dr_engines_running   = nullptr;
static XPLMDataRef dr_com1_freq         = nullptr;
static XPLMDataRef dr_com2_freq         = nullptr;
static XPLMDataRef dr_active_com        = nullptr;
static XPLMDataRef dr_aircraft_icao     = nullptr;

void init() {
    dr_latitude           = XPLMFindDataRef("sim/flightmodel/position/latitude");
    dr_longitude          = XPLMFindDataRef("sim/flightmodel/position/longitude");
    dr_altitude           = XPLMFindDataRef("sim/flightmodel/position/elevation");
    dr_groundspeed        = XPLMFindDataRef("sim/flightmodel/position/groundspeed");
    dr_indicated_airspeed = XPLMFindDataRef("sim/flightmodel/position/indicated_airspeed");
    dr_vertical_speed     = XPLMFindDataRef("sim/flightmodel/position/vh_ind_fpm");
    dr_heading_true       = XPLMFindDataRef("sim/flightmodel/position/true_psi");
    dr_on_ground          = XPLMFindDataRef("sim/flightmodel/failures/onground_any");
    dr_engines_running    = XPLMFindDataRef("sim/flightmodel/engine/ENGN_running");
    dr_com1_freq          = XPLMFindDataRef("sim/cockpit2/radios/actuators/com1_frequency_hz_833");
    dr_com2_freq          = XPLMFindDataRef("sim/cockpit2/radios/actuators/com2_frequency_hz_833");
    dr_active_com         = XPLMFindDataRef("sim/cockpit2/radios/actuators/audio_com_selection");
    dr_aircraft_icao      = XPLMFindDataRef("sim/aircraft/view/acf_ICAO");
}

void stop() {
    ctx = XPlaneContext{};
}

void update() {
    if (dr_latitude)           ctx.latitude             = XPLMGetDatad(dr_latitude);
    if (dr_longitude)          ctx.longitude            = XPLMGetDatad(dr_longitude);
    if (dr_altitude)           ctx.altitude_ft_msl      = static_cast<float>(XPLMGetDatad(dr_altitude) * 3.28084);
    if (dr_groundspeed)        ctx.groundspeed_kts      = XPLMGetDataf(dr_groundspeed) * 1.94384f;
    if (dr_indicated_airspeed) ctx.indicated_airspeed_kts = XPLMGetDataf(dr_indicated_airspeed);
    if (dr_vertical_speed)     ctx.vertical_speed_fpm   = XPLMGetDataf(dr_vertical_speed);
    if (dr_heading_true)       ctx.heading_true         = XPLMGetDataf(dr_heading_true);
    if (dr_on_ground)          ctx.on_ground            = XPLMGetDatai(dr_on_ground) != 0;

    if (dr_engines_running) {
        int running[8] = {};
        XPLMGetDatavi(dr_engines_running, running, 0, 8);
        ctx.engines_running = false;
        for (int i = 0; i < 8; ++i) {
            if (running[i]) { ctx.engines_running = true; break; }
        }
    }

    if (dr_com1_freq)  ctx.com1_freq_mhz = XPLMGetDatai(dr_com1_freq) / 1000.0f;
    if (dr_com2_freq)  ctx.com2_freq_mhz = XPLMGetDatai(dr_com2_freq) / 1000.0f;
    if (dr_active_com) ctx.active_com    = XPLMGetDatai(dr_active_com);

    if (dr_aircraft_icao) {
        char buf[64] = {};
        XPLMGetDatab(dr_aircraft_icao, buf, 0, sizeof(buf) - 1);
        ctx.aircraft_icao = buf;
    }

    // Stub: nearest_airport_id and is_towered_airport — implement in M1
    ctx.nearest_airport_id = "";
    ctx.is_towered_airport = false;
}

const XPlaneContext& get() {
    return ctx;
}

}  // namespace xplane_context
