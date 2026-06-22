/*
 * xp_wellys_atc - headless IFR test CLI
 *
 * Usage:
 *   atc_ifr_repl              — starts with LFMN default context
 *   atc_ifr_repl <cifp_dir>   — override CIFP directory on the command line
 *
 * Default LFMN scenario:
 *   Aircraft near ABDIL STAR entry, FL115, heading 103, GS 200 kt, VS -800 fpm.
 *   Destination: LFMN, QNH 1024, wind 165/05 (favours runway 04L).
 *   CIFP: auto-detected from XP_CIFP_DIR env, or ~/X-Plane 12/Custom Data/CIFP.
 *
 * The REPL exposes poll_approach, fly, goto, jump_*, and say so the full
 * IFR approach sequence can be simulated without X-Plane running.
 */

#include "ifr_repl.hpp"

#include "atc/atc_state_machine.hpp"
#include "atc/atc_templates.hpp"
#include "atc/flight_phase.hpp"
#include "data/airport_vrps.hpp"
#include "data/simbrief_ofp.hpp"
#include "core/xplane_context.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>

// Try to locate the X-Plane 12 CIFP directory automatically.
// Priority: XP_CIFP_DIR env var > ~/X-Plane 12/Custom Data/CIFP.
static std::string detect_cifp_dir() {
  if (const char *v = std::getenv("XP_CIFP_DIR"))
    return v;
  if (const char *home = std::getenv("HOME")) {
    std::string candidate = std::string(home) + "/X-Plane 12/Custom Data/CIFP";
    return candidate;
  }
  return "";
}

static xplane_context::XPlaneContext lfmn_context(const std::string &cifp_dir) {
  using xplane_context::FrequencyType;

  xplane_context::XPlaneContext ctx;

  // Aircraft position: near ABDIL (ABDI8R STAR entry for LFMN), west of Nice.
  ctx.latitude         = 43.65;
  ctx.longitude        = 6.20;
  ctx.altitude_ft_msl  = 11500.0f;
  ctx.pressure_alt_ft  = 11500.0f;
  ctx.heading_true     = 103.0f;  // east toward LFMN
  ctx.groundspeed_kts  = 200.0f;
  ctx.vertical_speed_fpm = -800.0f;
  ctx.height_agl_ft    = 11000.0f;
  ctx.on_ground        = false;
  ctx.avionics_on      = true;
  ctx.com_radio_powered = true;

  // Radio — LFMN Approach
  ctx.com1_freq_mhz  = 120.160f;
  ctx.active_com     = 1;
  ctx.frequency_type = FrequencyType::APPROACH;

  // Weather: QNH 1024, wind 350/10 kt (northerly Tramontane — favours runway 04L at LFMN)
  ctx.qnh_hpa            = 1024;
  ctx.qnh_inhg           = 30.24f;
  ctx.wind_direction_deg = 350.0f;
  ctx.wind_speed_kt      = 10.0f;
  ctx.visibility_m       = 9999.0f;
  ctx.temperature_c      = 20.0f;
  ctx.dewpoint_c         = 10.0f;

  // IFR destination
  ctx.nearest_airport_id = "LFMN";
  ctx.is_towered_airport = true;
  ctx.ifr_destination    = "LFMN";
  ctx.aircraft_icao      = "B738";
  ctx.cifp_dir           = cifp_dir;

  return ctx;
}

int main(int argc, char **argv) {
  // Init engine modules (same order as atc_repl).
  atc_templates::init();
  flight_phase::init();
  atc_state_machine::init();
  airport_vrps::init();

  std::string cifp_dir = detect_cifp_dir();
  if (argc >= 2) cifp_dir = argv[1]; // explicit override

  auto ctx = lfmn_context(cifp_dir);

  // Pre-populate OFP with LFMN as destination and ABDIL as the last navlog fix
  // so training_jump_approach picks up the correct STAR via star_name_for_entry_fix.
  {
    simbrief_ofp::OfpData ofp;
    ofp.destination_icao  = "LFMN";
    ofp.destination_name  = "Nice";
    ofp.origin_icao       = "LFLP";
    ofp.valid             = true;
    // ABDIL is the ABDI8R STAR entry fix for LFMN 04L.
    simbrief_ofp::NavlogFix abdil;
    abdil.ident       = "ABDIL";
    abdil.via_airway  = "DCT";
    abdil.is_sid_star = true;
    ofp.navlog.push_back(abdil);
    simbrief_ofp::set(ofp);
  }

  if (cifp_dir.empty()) {
    std::fprintf(stderr,
        "Warning: CIFP directory not found.\n"
        "  Set XP_CIFP_DIR env var or pass it as an argument:\n"
        "  atc_ifr_repl \"/path/to/X-Plane 12/Custom Data/CIFP\"\n\n");
  } else {
    std::fprintf(stderr, "CIFP: %s\n", cifp_dir.c_str());
  }

  return ifr_repl::run(std::move(ctx), "November Romeo Charlie");
}
