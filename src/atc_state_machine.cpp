#include "atc_state_machine.hpp"
#include "settings.hpp"

#include <XPLMUtilities.h>

#include <cmath>
#include <cstdio>

namespace atc_state_machine {

static ATCState state_ = ATCState::IDLE;

// Helper: get callsign from message or settings fallback
static std::string get_callsign(const intent_parser::PilotMessage &msg) {
  if (!msg.callsign.empty())
    return msg.callsign;
  return settings::pilot_callsign();
}

// Helper: get runway from message or fallback
static std::string get_runway(const intent_parser::PilotMessage &msg) {
  if (!msg.runway.empty())
    return msg.runway;
  return "28"; // default placeholder
}

// Helper: format QNH from inHg
static std::string format_qnh(float inhg) {
  int hpa = static_cast<int>(std::round(inhg * 33.8639f));
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%d", hpa);
  return buf;
}

// Helper: format wind
static std::string format_wind(float dir, float spd) {
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%03.0f degrees %02.0f knots", dir, spd);
  return buf;
}

// Helper: airport name from ICAO (just return the ID for now)
static std::string airport_name(const xplane_context::XPlaneContext &ctx) {
  if (!ctx.nearest_airport_id.empty())
    return ctx.nearest_airport_id;
  return "Airport";
}

void init() { state_ = ATCState::IDLE; }

void stop() { state_ = ATCState::IDLE; }

void reset() {
  state_ = ATCState::IDLE;
  XPLMDebugString("[xp_wellys_atc] ATC state machine reset to IDLE\n");
}

ATCState get_state() { return state_; }

const char *state_name(ATCState state) {
  switch (state) {
  case ATCState::IDLE:
    return "IDLE";
  case ATCState::GROUND_CONTACT:
    return "GROUND_CONTACT";
  case ATCState::TAXI_CLEARED:
    return "TAXI_CLEARED";
  case ATCState::TOWER_CONTACT:
    return "TOWER_CONTACT";
  case ATCState::DEPARTURE_CLEARED:
    return "DEPARTURE_CLEARED";
  case ATCState::PATTERN_ENTRY:
    return "PATTERN_ENTRY";
  case ATCState::LANDING_CLEARED:
    return "LANDING_CLEARED";
  case ATCState::UNICOM_ACTIVE:
    return "UNICOM_ACTIVE";
  }
  return "UNKNOWN";
}

ATCResponse process(const intent_parser::PilotMessage &msg,
                    const xplane_context::XPlaneContext &ctx) {
  using Intent = intent_parser::PilotIntent;
  ATCResponse resp;

  std::string cs = get_callsign(msg);
  std::string rwy = get_runway(msg);
  std::string qnh = format_qnh(ctx.qnh_inhg);
  std::string wind = format_wind(ctx.wind_direction_deg, ctx.wind_speed_kt);
  std::string apt = airport_name(ctx);

  using FT = xplane_context::FrequencyType;

  // Non-towered airport OR Unicom/CTAF frequency: force UNICOM flow
  bool unicom_flow = !ctx.is_towered_airport ||
                     ctx.frequency_type == FT::UNICOM ||
                     ctx.frequency_type == FT::CTAF;

  if (unicom_flow) {
    // Traffic awareness only — no clearances
    std::string position;
    if (ctx.on_ground) {
      position = "on the ground at " + apt;
    } else {
      // Try to extract position from transcript
      std::string lower = msg.raw_transcript;
      for (auto &c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      if (lower.find("downwind") != std::string::npos)
        position = "downwind runway " + rwy;
      else if (lower.find("base") != std::string::npos)
        position = "base runway " + rwy;
      else if (lower.find("final") != std::string::npos)
        position = "final runway " + rwy;
      else if (lower.find("crosswind") != std::string::npos)
        position = "crosswind runway " + rwy;
      else if (lower.find("upwind") != std::string::npos)
        position = "upwind runway " + rwy;
      else
        position = "in the pattern at " + apt;
    }

    resp.text = "Traffic in the area, " + cs + " reported " + position + ".";
    resp.next_state = ATCState::IDLE; // immediately back to IDLE
    state_ = ATCState::IDLE;

    char log[256];
    std::snprintf(log, sizeof(log),
                  "[xp_wellys_atc] ATC state: UNICOM_ACTIVE -> IDLE "
                  "(non-towered/CTAF)\n");
    XPLMDebugString(log);
    return resp;
  }

  // Frequency-based state validation at towered airports
  if (ctx.frequency_type == FT::GROUND && state_ != ATCState::IDLE &&
      state_ != ATCState::GROUND_CONTACT && state_ != ATCState::TAXI_CLEARED) {
    // On ground frequency but in tower state — reset
    state_ = ATCState::IDLE;
  }
  if (ctx.frequency_type == FT::TOWER && state_ != ATCState::IDLE &&
      state_ != ATCState::TOWER_CONTACT &&
      state_ != ATCState::DEPARTURE_CLEARED &&
      state_ != ATCState::PATTERN_ENTRY &&
      state_ != ATCState::LANDING_CLEARED) {
    // On tower frequency but in ground state — skip to tower
    state_ = ATCState::IDLE;
  }

  switch (state_) {
  case ATCState::IDLE: {
    if (msg.intent == Intent::INITIAL_CALL) {
      // Check if targeting ground/delivery → GROUND_CONTACT
      std::string lower = msg.raw_transcript;
      for (auto &c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

      if (lower.find("ground") != std::string::npos ||
          lower.find("delivery") != std::string::npos) {
        resp.text = cs + ", " + apt + " Ground, information Alpha, runway " +
                    rwy + ", QNH " + qnh + ", taxi to holding point runway " +
                    rwy + " via Alpha.";
        resp.next_state = ATCState::GROUND_CONTACT;
        resp.requires_readback = true;
      }
      // Check if targeting tower → TOWER_CONTACT (e.g., arriving aircraft)
      else if (lower.find("tower") != std::string::npos) {
        resp.text = cs + ", " + apt + " Tower, runway " + rwy +
                    ", hold short, number one.";
        resp.next_state = ATCState::TOWER_CONTACT;
        resp.requires_readback = false;
      }
    }
    break;
  }

  case ATCState::GROUND_CONTACT: {
    if (msg.intent == Intent::REQUEST_TAXI) {
      resp.text = cs + ", taxi to holding point runway " + rwy +
                  " via Alpha, QNH " + qnh + ".";
      resp.next_state = ATCState::TAXI_CLEARED;
      resp.requires_readback = true;
    }
    break;
  }

  case ATCState::TAXI_CLEARED: {
    if (msg.intent == Intent::INITIAL_CALL) {
      std::string lower = msg.raw_transcript;
      for (auto &c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

      if (lower.find("tower") != std::string::npos) {
        resp.text = cs + ", " + apt + " Tower, runway " + rwy +
                    ", hold short, number one.";
        resp.next_state = ATCState::TOWER_CONTACT;
        resp.requires_readback = false;
      }
    }
    break;
  }

  case ATCState::TOWER_CONTACT: {
    if (msg.intent == Intent::READY_FOR_DEPARTURE) {
      resp.text =
          cs + ", runway " + rwy + ", cleared for takeoff, wind " + wind + ".";
      resp.next_state = ATCState::DEPARTURE_CLEARED;
      resp.requires_readback = true;
    } else if (msg.intent == Intent::REQUEST_LANDING ||
               (msg.intent == Intent::REPORT_POSITION && !ctx.on_ground)) {
      resp.text = cs + ", number one, runway " + rwy + ", report final.";
      resp.next_state = ATCState::PATTERN_ENTRY;
      resp.requires_readback = false;
    }
    break;
  }

  case ATCState::DEPARTURE_CLEARED: {
    if (msg.intent == Intent::READBACK) {
      resp.text = cs + ", frequency change approved, good day.";
      resp.next_state = ATCState::IDLE;
      resp.requires_readback = false;
    }
    break;
  }

  case ATCState::PATTERN_ENTRY: {
    if (msg.intent == Intent::REPORT_POSITION) {
      std::string lower = msg.raw_transcript;
      for (auto &c : lower)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

      if (lower.find("final") != std::string::npos) {
        resp.text =
            cs + ", runway " + rwy + ", cleared to land, wind " + wind + ".";
        resp.next_state = ATCState::LANDING_CLEARED;
        resp.requires_readback = true;
      }
    }
    break;
  }

  case ATCState::LANDING_CLEARED: {
    if (msg.intent == Intent::RUNWAY_VACATED) {
      resp.text = cs + ", contact ground on 121.9, good day.";
      resp.next_state = ATCState::IDLE;
      resp.requires_readback = false;
    }
    break;
  }

  case ATCState::UNICOM_ACTIVE: {
    // Should not reach here — handled by unicom_flow above
    resp.next_state = ATCState::IDLE;
    state_ = ATCState::IDLE;
    break;
  }
  }

  // Apply state transition if we have a response
  if (!resp.text.empty()) {
    char log[256];
    std::snprintf(log, sizeof(log), "[xp_wellys_atc] ATC state: %s -> %s\n",
                  state_name(state_), state_name(resp.next_state));
    XPLMDebugString(log);
    state_ = resp.next_state;
  }

  return resp;
}

} // namespace atc_state_machine
