/*
 * xp_wellys_atc - headless IFR test CLI
 *
 * REPL for IFR approach simulation. The aircraft starts near the STAR
 * entry and the user can fly it step by step (fly/goto/poll) while
 * watching the ATC step-down clearances fire in sequence.
 *
 * Context lives in xplane_context::g_cli_ctx (same stub as atc_repl).
 * A global now_secs counter advances with every poll/fly call so the
 * engine's cooldown logic behaves as in the live plugin.
 */

#include "ifr_repl.hpp"

#include "atc/atc_state_machine.hpp"
#include "atc/atc_templates.hpp"
#include "atc/engine.hpp"
#include "atc/flight_phase.hpp"
#include "atc/intent_parser.hpp"
#include "core/xplane_context.hpp"
#include "data/cifp_reader.hpp"
#include "data/simbrief_ofp.hpp"
#include "persistence/settings.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <exception>
#include <iostream>
#include <string>
#include <unordered_map>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

namespace xplane_context {
extern XPlaneContext g_cli_ctx;
}

namespace ifr_repl {
namespace {

using xplane_context::FrequencyType;

// Running simulated time (seconds). Advanced by poll/fly commands.
static double g_now_secs = 0.0;

// ── String helpers ────────────────────────────────────────────────────

std::string trim(std::string s) {
  auto notsp = [](unsigned char c) { return !std::isspace(c); };
  s.erase(s.begin(), std::find_if(s.begin(), s.end(), notsp));
  s.erase(std::find_if(s.rbegin(), s.rend(), notsp).base(), s.end());
  return s;
}

std::pair<std::string, std::string> split_first(const std::string &line) {
  auto sp = line.find_first_of(" \t");
  if (sp == std::string::npos) return {line, ""};
  return {line.substr(0, sp), trim(line.substr(sp + 1))};
}

bool parse_freq_type(const std::string &s, FrequencyType &out) {
  static const std::unordered_map<std::string, FrequencyType> kMap{
      {"UNKNOWN", FrequencyType::UNKNOWN},
      {"DELIVERY", FrequencyType::DELIVERY},
      {"GROUND", FrequencyType::GROUND},
      {"TOWER", FrequencyType::TOWER},
      {"APPROACH", FrequencyType::APPROACH},
      {"DEPARTURE", FrequencyType::DEPARTURE},
      {"UNICOM", FrequencyType::UNICOM},
      {"ATIS", FrequencyType::ATIS},
  };
  std::string up = s;
  std::transform(up.begin(), up.end(), up.begin(),
                 [](unsigned char c) { return std::toupper(c); });
  auto it = kMap.find(up);
  if (it == kMap.end()) return false;
  out = it->second;
  return true;
}

// ── Position math (flat-earth, good to ~200 NM) ─────────────────────

void advance_position(xplane_context::XPlaneContext &ctx, float nm) {
  double hdg_rad = ctx.heading_true * M_PI / 180.0;
  double lat_rad = ctx.latitude * M_PI / 180.0;
  ctx.latitude  += nm * std::cos(hdg_rad) / 60.0;
  ctx.longitude += nm * std::sin(hdg_rad) / (60.0 * std::cos(lat_rad));
}

// ── Poll helper: run all frame-driven IFR polls for one dt step ──────

void run_polls(float dt) {
  auto &ctx = xplane_context::g_cli_ctx;
  g_now_secs += dt;
  ctx.now_secs = g_now_secs;

  auto emit = [](const char *tag, const std::string &text) {
    if (!text.empty())
      std::printf("ATC [%s]: %s\n", tag, text.c_str());
  };

  // Route tracker must run BEFORE poll_approach so s_route_fix_idx is
  // up-to-date when poll_approach checks (s_route_fix_idx > s_faf_route_idx)
  // for the Tower handoff. This matches the order in atc_session::update().
  //
  // The tracker's internal tick accumulates 1/60 per call and fires at 1.0
  // (i.e., ~once per second at 60 FPS). Simulate dt seconds worth of frames
  // so proximity events fire correctly even in large poll steps.
  {
    int rt_calls = std::max(60, static_cast<int>(std::ceil(dt * 60.0f)));
    for (int i = 0; i < rt_calls; ++i) {
      std::string t = engine::poll_route_tracker(ctx);
      if (!t.empty())
        std::printf("-- %s --\n", t.c_str());
    }
  }

  std::string out;
  if (engine::poll_departure_handoff(ctx, dt, &out)) { emit("dep", out); out.clear(); }
  if (engine::poll_sid_climb(ctx, dt, &out))         { emit("sid", out); out.clear(); }
  if (engine::poll_enroute(ctx, dt, &out))           { emit("enroute", out); out.clear(); }
  if (engine::poll_approach(ctx, dt, &out))          { emit("approach", out); out.clear(); }
  if (engine::poll_approach_alignment(ctx, dt, &out)){ emit("align", out); out.clear(); }
  if (engine::poll_readback_reminder(ctx, g_now_secs, &out)) { emit("readback", out); out.clear(); }
  if (engine::poll_go_around(ctx, g_now_secs, &out)) { emit("go_around", out); out.clear(); }
}

// ── Command handlers ──────────────────────────────────────────────────

void cmd_say(const std::string &callsign, const std::string &text) {
  if (text.empty()) {
    std::fprintf(stderr, "Usage: say <transcript>\n");
    return;
  }
  auto &ctx = xplane_context::g_cli_ctx;
  ctx.now_secs = g_now_secs;
  engine::Input in{text, 1.0f, &ctx, callsign, g_now_secs};
  engine::process_transcript(std::move(in), [](engine::Output out) {
    std::printf("PILOT : %s\n", out.parsed.raw_transcript.c_str());
    std::printf("INTENT: %s (%.2f)\n",
                intent_parser::intent_name(out.parsed.intent),
                out.parsed.confidence);
    std::printf("ATC   : %s\n",
                out.response_text.empty() ? "(silent)" : out.response_text.c_str());
    std::printf("STATE : %s\n",
                atc_state_machine::state_name(atc_state_machine::get_state()));
  });
}

void cmd_set(std::string &callsign, const std::string &rest) {
  auto [field, value] = split_first(rest);
  if (field.empty() || value.empty()) {
    std::fprintf(stderr, "Usage: set <field> <value>  (try 'help')\n");
    return;
  }
  auto &ctx = xplane_context::g_cli_ctx;
  try {
    if (field == "lat") {
      ctx.latitude = std::stod(value);
    } else if (field == "lon") {
      ctx.longitude = std::stod(value);
    } else if (field == "alt") {
      ctx.altitude_ft_msl = std::stof(value);
      ctx.pressure_alt_ft = std::stof(value); // sync both unless pa given separately
    } else if (field == "pa") {
      ctx.pressure_alt_ft = std::stof(value);
    } else if (field == "heading") {
      ctx.heading_true = std::stof(value);
    } else if (field == "gs") {
      ctx.groundspeed_kts = std::stof(value);
    } else if (field == "vs") {
      ctx.vertical_speed_fpm = std::stof(value);
    } else if (field == "cifp_dir") {
      ctx.cifp_dir = value;
    } else if (field == "dest") {
      std::string up = value;
      std::transform(up.begin(), up.end(), up.begin(),
                     [](unsigned char c) { return std::toupper(c); });
      ctx.ifr_destination = up;
      // Mirror to OFP so training_jump_approach can pick up dest ICAO.
      auto ofp = simbrief_ofp::get();
      ofp.destination_icao = up;
      ofp.valid = true;
      simbrief_ofp::set(ofp);
    } else if (field == "qnh") {
      ctx.qnh_hpa = static_cast<int>(std::stof(value));
    } else if (field == "wind_dir") {
      ctx.wind_direction_deg = std::stof(value);
    } else if (field == "wind_kt") {
      ctx.wind_speed_kt = std::stof(value);
    } else if (field == "visibility") {
      ctx.visibility_m = std::stof(value);
    } else if (field == "airport") {
      std::string up = value;
      std::transform(up.begin(), up.end(), up.begin(),
                     [](unsigned char c) { return std::toupper(c); });
      ctx.nearest_airport_id = up;
    } else if (field == "com") {
      ctx.com1_freq_mhz = std::stof(value);
      ctx.active_com = 1;
    } else if (field == "freq_type") {
      FrequencyType ft;
      if (!parse_freq_type(value, ft))
        throw std::runtime_error("unknown freq_type");
      ctx.frequency_type = ft;
    } else if (field == "runway") {
      ctx.active_runway = value;
    } else if (field == "callsign") {
      callsign = value;
      settings::set_pilot_callsign_raw(value);
    } else if (field == "region") {
      std::string up = value;
      std::transform(up.begin(), up.end(), up.begin(),
                     [](unsigned char c) { return std::toupper(c); });
      settings::set_atc_profile(up);
      atc_templates::reload();
      flight_phase::reload();
    } else if (field == "navlog_fix") {
      // Append a navlog fix: set navlog_fix ABDIL
      auto ofp = simbrief_ofp::get();
      simbrief_ofp::NavlogFix f;
      f.ident = value;
      f.is_sid_star = true;
      ofp.navlog.push_back(f);
      simbrief_ofp::set(ofp);
    } else if (field == "approach_desig") {
      // Force a specific approach procedure, e.g. "set approach_desig I04LZ"
      auto ofp = simbrief_ofp::get();
      std::string up = value;
      std::transform(up.begin(), up.end(), up.begin(),
                     [](unsigned char c) { return std::toupper(c); });
      ofp.preferred_approach_designator = up;
      simbrief_ofp::set(ofp);
    } else {
      std::fprintf(stderr, "Unknown field '%s' (try 'help')\n", field.c_str());
      return;
    }
    std::fprintf(stderr, "set %s = %s\n", field.c_str(), value.c_str());
  } catch (const std::exception &e) {
    std::fprintf(stderr, "Error setting %s: %s\n", field.c_str(), e.what());
  }
}

void cmd_poll(const std::string &rest) {
  float dt = 5.0f;
  if (!rest.empty()) {
    try { dt = std::stof(rest); } catch (...) {}
  }
  run_polls(dt);
  std::printf("[t=%.0fs] lat=%.4f lon=%.4f alt=%.0fft pa=%.0fft\n",
              g_now_secs,
              xplane_context::g_cli_ctx.latitude,
              xplane_context::g_cli_ctx.longitude,
              xplane_context::g_cli_ctx.altitude_ft_msl,
              xplane_context::g_cli_ctx.pressure_alt_ft);
}

void cmd_fly(const std::string &rest) {
  if (rest.empty()) {
    std::fprintf(stderr, "Usage: fly <nm>\n");
    return;
  }
  auto &ctx = xplane_context::g_cli_ctx;
  float total_nm;
  try { total_nm = std::stof(rest); } catch (...) {
    std::fprintf(stderr, "Invalid distance\n"); return;
  }
  float gs = std::max(ctx.groundspeed_kts, 1.0f);
  float dt = 5.0f; // seconds per simulation step
  float d_nm = gs * dt / 3600.0f;
  int steps = std::max(1, static_cast<int>(std::ceil(total_nm / d_nm)));

  std::fprintf(stderr, "Flying %.1f NM (hdg=%.0f gs=%.0f kt, %d steps of %.1f s)...\n",
               total_nm, ctx.heading_true, gs, steps, dt);

  float flown = 0.0f;
  for (int i = 0; i < steps; ++i) {
    float step = std::min(d_nm, total_nm - flown);
    advance_position(ctx, step);
    float alt_change = ctx.vertical_speed_fpm * dt / 60.0f;
    ctx.altitude_ft_msl += alt_change;
    ctx.pressure_alt_ft += alt_change;
    flown += step;
    run_polls(dt);
  }
  std::printf("[t=%.0fs] flew %.1f NM -> lat=%.4f lon=%.4f alt=%.0fft pa=%.0fft\n",
              g_now_secs, total_nm,
              ctx.latitude, ctx.longitude,
              ctx.altitude_ft_msl, ctx.pressure_alt_ft);
}

void cmd_goto(const std::string &fix) {
  if (fix.empty()) {
    std::fprintf(stderr, "Usage: goto <FIXNAME>\n");
    return;
  }
  auto &ctx = xplane_context::g_cli_ctx;
  if (ctx.cifp_dir.empty()) {
    std::fprintf(stderr, "cifp_dir not set — use 'set cifp_dir <path>'\n");
    return;
  }
  std::string up = fix;
  std::transform(up.begin(), up.end(), up.begin(),
                 [](unsigned char c) { return std::toupper(c); });
  auto pos = cifp_reader::lookup_fix_positions(ctx.cifp_dir, {up},
                                               ctx.nearest_airport_id);
  auto it = pos.find(up);
  if (it == pos.end()) {
    std::fprintf(stderr, "Fix '%s' not found in earth_fix.dat\n", up.c_str());
    return;
  }
  ctx.latitude  = it->second.first;
  ctx.longitude = it->second.second;
  std::printf("Teleported to %s: lat=%.4f lon=%.4f\n",
              up.c_str(), ctx.latitude, ctx.longitude);
}

void cmd_jump(const std::string &rest) {
  auto [sub, arg] = split_first(rest);
  if (sub == "approach") {
    engine::training_jump_approach();
    std::printf("Jumped to IFR_APPROACH_CONTACT (dest=%s)\n",
                xplane_context::g_cli_ctx.ifr_destination.c_str());
    std::printf("STATE : %s\n",
                atc_state_machine::state_name(atc_state_machine::get_state()));
  } else if (sub == "enroute") {
    int alt = 0;
    try { alt = std::stoi(arg); } catch (...) {}
    if (alt <= 0) {
      std::fprintf(stderr, "Usage: jump enroute <alt_ft>\n");
      return;
    }
    engine::training_jump_enroute(alt);
    std::printf("Jumped to IFR_ENROUTE_CRUISE at %d ft\n", alt);
    std::printf("STATE : %s\n",
                atc_state_machine::state_name(atc_state_machine::get_state()));
  } else if (sub == "predep") {
    engine::training_jump_predep();
    std::printf("Jumped to IFR_PREDEP_CLEARANCE\n");
    std::printf("STATE : %s\n",
                atc_state_machine::state_name(atc_state_machine::get_state()));
  } else {
    std::fprintf(stderr, "Usage: jump approach|enroute <alt_ft>|predep\n");
  }
}

void cmd_state(const std::string &callsign) {
  const auto &ctx = xplane_context::g_cli_ctx;
  auto ofp = simbrief_ofp::get();
  std::printf("Callsign:  %s\n", callsign.c_str());
  std::printf("Airport:   %s  towered=%s  runway=%s\n",
              ctx.nearest_airport_id.c_str(),
              ctx.is_towered_airport ? "yes" : "no",
              ctx.active_runway.c_str());
  std::printf("Position:  lat=%.4f lon=%.4f\n", ctx.latitude, ctx.longitude);
  std::printf("Alt:       %.0f ft MSL  PA=%.0f ft  AGL=%.0f ft\n",
              ctx.altitude_ft_msl, ctx.pressure_alt_ft, ctx.height_agl_ft);
  std::printf("Motion:    hdg=%.0f  gs=%.0f kt  vs=%+.0f fpm\n",
              ctx.heading_true, ctx.groundspeed_kts, ctx.vertical_speed_fpm);
  std::printf("Radio:     COM1=%.3f MHz  freq_type=%s\n",
              ctx.com1_freq_mhz,
              xplane_context::frequency_type_name(ctx.frequency_type));
  std::printf("Weather:   QNH=%d hPa  wind=%.0f/%.0f kt  vis=%.0f m\n",
              ctx.qnh_hpa, ctx.wind_direction_deg, ctx.wind_speed_kt,
              ctx.visibility_m);
  std::printf("IFR dest:  %s  CIFP=%s\n",
              ctx.ifr_destination.empty() ? "(none)" : ctx.ifr_destination.c_str(),
              ctx.cifp_dir.empty() ? "(none)" : "set");
  std::printf("OFP:       dest=%s  valid=%s  navlog=%zu fixes\n",
              ofp.destination_icao.empty() ? "(none)" : ofp.destination_icao.c_str(),
              ofp.valid ? "yes" : "no",
              ofp.navlog.size());
  std::printf("ATC state: %s\n",
              atc_state_machine::state_name(atc_state_machine::get_state()));
  std::printf("Time:      t=%.0f s\n", g_now_secs);
  std::printf("Region:    %s\n", settings::atc_profile().c_str());
}

void cmd_reset() {
  atc_state_machine::reset();
  engine::reset();
  g_now_secs = 0.0;
  xplane_context::g_cli_ctx.now_secs = 0.0;
  std::fprintf(stderr, "Engine state reset (context unchanged, t=0).\n");
}

void cmd_help() {
  std::printf(
      "Commands:\n"
      "  say <text>              Process a pilot transcript\n"
      "  poll [dt=5]             Advance dt seconds and run all IFR polls\n"
      "  fly <nm>                Fly NM at current hdg/gs/vs, polling every 5 s\n"
      "  goto <FIXNAME>          Teleport aircraft to fix (earth_fix.dat lookup)\n"
      "  jump approach           Jump to IFR_APPROACH_CONTACT state\n"
      "  jump enroute <alt_ft>   Jump to IFR_ENROUTE_CRUISE state\n"
      "  jump predep             Jump to IFR_PREDEP_CLEARANCE state\n"
      "  set <field> <value>     Modify context (see fields below)\n"
      "  state                   Show full context + ATC state\n"
      "  reset                   Reset engine state (context unchanged, t=0)\n"
      "  help                    This message\n"
      "  quit                    Exit\n"
      "\n"
      "Set fields:\n"
      "  lat <deg>          Aircraft latitude\n"
      "  lon <deg>          Aircraft longitude\n"
      "  alt <ft>           True altitude (also syncs pressure_alt)\n"
      "  pa <ft>            Pressure altitude only\n"
      "  heading <deg>      True heading\n"
      "  gs <kt>            Ground speed\n"
      "  vs <fpm>           Vertical speed (negative = descending)\n"
      "  cifp_dir <path>    Path to X-Plane CIFP directory\n"
      "  dest <ICAO>        IFR destination (also sets OFP.destination_icao)\n"
      "  qnh <hpa>          QNH pressure in hPa\n"
      "  wind_dir <deg>     Wind direction (true) — used for runway selection\n"
      "  wind_kt <kt>       Wind speed\n"
      "  visibility <m>     Visibility in metres\n"
      "  airport <ICAO>     Nearest airport\n"
      "  com <MHz>          COM1 frequency\n"
      "  freq_type <TYPE>   APPROACH|TOWER|GROUND|DEPARTURE|UNICOM|DELIVERY\n"
      "  runway <id>        Active runway (e.g. 04L)\n"
      "  callsign <text>    Pilot callsign\n"
      "  region EU|US|DE    ATC phraseology region\n"
      "  navlog_fix <IDENT> Append a fix ident to the OFP navlog\n"
      "  approach_desig <D> Force a specific approach (e.g. I04LZ) over best_approach()\n"
      "\n"
      "Typical IFR approach test flow (LFMN):\n"
      "  jump approach\n"
      "  say november romeo charlie, inbound, flight level one one five\n"
      "  fly 20          <- flies toward dest, watch step-down clearances fire\n"
      "  goto FN04A      <- teleport to FAF, then poll for Tower handoff\n"
      "  poll 5\n");
}

} // namespace

int run(xplane_context::XPlaneContext ctx, std::string callsign) {
  ctx.avionics_on = true;
  ctx.com_radio_powered = true;
  if (ctx.aircraft_icao.empty()) ctx.aircraft_icao = "B738";
  if (!callsign.empty()) settings::set_pilot_callsign_raw(callsign);

  // Mirror OFP destination from ctx so training_jump_approach works on start.
  if (!ctx.ifr_destination.empty()) {
    auto ofp = simbrief_ofp::get();
    if (ofp.destination_icao.empty()) {
      ofp.destination_icao = ctx.ifr_destination;
      ofp.valid = true;
      simbrief_ofp::set(ofp);
    }
  }

  xplane_context::g_cli_ctx = std::move(ctx);
  atc_state_machine::reset();
  engine::reset();
  g_now_secs = 0.0;

  std::fprintf(
      stderr,
      "atc_ifr_repl — type 'help' for commands, 'quit' or Ctrl+D to exit.\n");
  cmd_state(callsign);

  std::string line;
  while (true) {
    std::fprintf(stderr, "\n> ");
    if (!std::getline(std::cin, line)) {
      std::fprintf(stderr, "\n");
      return 0;
    }
    line = trim(std::move(line));
    if (line.empty()) continue;

    auto [cmd, rest] = split_first(line);
    if (cmd == "say")
      cmd_say(callsign, rest);
    else if (cmd == "set")
      cmd_set(callsign, rest);
    else if (cmd == "poll")
      cmd_poll(rest);
    else if (cmd == "fly")
      cmd_fly(rest);
    else if (cmd == "goto")
      cmd_goto(rest);
    else if (cmd == "jump")
      cmd_jump(rest);
    else if (cmd == "state")
      cmd_state(callsign);
    else if (cmd == "reset")
      cmd_reset();
    else if (cmd == "help")
      cmd_help();
    else if (cmd == "quit" || cmd == "exit")
      return 0;
    else
      std::fprintf(stderr, "Unknown command: %s (try 'help')\n", cmd.c_str());
  }
}

} // namespace ifr_repl
