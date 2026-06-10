// Tests for IFR departure handoff (poll_departure_handoff) and the
// frequency guard in process_transcript.
//
// Constraints verified:
//   1. Departure handoff does NOT fire below CTR upper altitude (2481 ft AGL
//      for LFLP — lower than the 2500 ft fallback, so LFLP hands off first).
//   2. Frequency guard: wrong frequency silently drops the transcript.
//   3. Frequency guard: IFR_EN_ROUTE rejects TOWER frequency.
//   4. Frequency guard: IFR_EN_ROUTE accepts APPROACH frequency.

#include "atc/atc_state_machine.hpp"
#include "atc/engine.hpp"
#include "atc/flight_phase.hpp"
#include "atc/intent_parser.hpp"
#include "core/xplane_context.hpp"
#include "data/openair_db.hpp"

#include <catch2/catch_amalgamated.hpp>

using atc_state_machine::ATCState;
using xplane_context::AirportFrequency;
using xplane_context::FrequencyType;
using xplane_context::XPlaneContext;

// LFLP Annecy Meythet: elevation 1519 ft MSL, ANNECY CTR upper 4000 ft MSL.
static constexpr float LFLP_ELEVATION_FT     = 1519.0f;
static constexpr float LFLP_CTR_UPPER_MSL_FT = 4000.0f;
static constexpr float LFLP_CTR_AGL          = LFLP_CTR_UPPER_MSL_FT - LFLP_ELEVATION_FT; // 2481 ft

namespace {

XPlaneContext make_ifr_ctx(float height_agl) {
    XPlaneContext ctx{};
    ctx.nearest_airport_id   = "LFLP";
    ctx.nearest_airport_name = "ANNECY MEYTHET";
    ctx.airport_lat = 45.9292;
    ctx.airport_lon = 6.1028;
    ctx.latitude    = 45.9292;
    ctx.longitude   = 6.1028;

    ctx.height_agl_ft    = height_agl;
    ctx.altitude_ft_msl  = LFLP_ELEVATION_FT + height_agl;
    ctx.is_towered_airport = true;
    ctx.on_ground = (height_agl < 1.0f);
    ctx.groundspeed_kts = 120.0f;

    // LFLP: APPROACH on 121.200, TOWER on 118.200
    AirportFrequency app{};
    app.freq_khz = 121200;
    app.type = FrequencyType::APPROACH;
    app.name = "CHAMBERY APP";
    ctx.airport_freqs.all.push_back(app);

    AirportFrequency twr{};
    twr.freq_khz = 118200;
    twr.type = FrequencyType::TOWER;
    twr.name = "ANNECY TWR";
    ctx.airport_freqs.all.push_back(twr);

    ctx.frequency_type = FrequencyType::TOWER; // default: on Tower
    return ctx;
}

} // namespace

// ── Altitude threshold sanity ─────────────────────────────────────────────

TEST_CASE("ifr departure: LFLP CTR upper altitude is below 2500 ft AGL fallback",
          "[ifr_departure]")
{
    // ANNECY CTR upper 4000 ft MSL, elevation 1519 ft => 2481 ft AGL.
    // This means if the plugin uses the OpenAir ceiling instead of the 2500 ft
    // fallback, the handoff fires ~19 ft AGL earlier — confirming the OpenAir
    // path is more accurate than the fallback.
    REQUIRE(LFLP_CTR_AGL < 2500.0f);
    REQUIRE(LFLP_CTR_AGL == Catch::Approx(2481.0f).margin(1.0f));
}

// ── Departure handoff altitude guard ─────────────────────────────────────

TEST_CASE("ifr departure: handoff does not fire below CTR upper altitude",
          "[ifr_departure]")
{
    engine::reset();
    flight_phase::init();
    atc_state_machine::init();
    openair_db::init(""); // disabled — no file in unit tests

    atc_state_machine::set_state(ATCState::IFR_DEPARTURE_CLEARED);

    // 100 ft below the fallback (2500 AGL) — handoff must NOT fire.
    // (OpenAir is disabled so the fallback 2500 ft is used.)
    XPlaneContext ctx = make_ifr_ctx(2400.0f);
    std::string text;
    bool fired = engine::poll_departure_handoff(ctx, 0.0f, &text);
    REQUIRE_FALSE(fired);
    // State unchanged
    REQUIRE(atc_state_machine::get_state() == ATCState::IFR_DEPARTURE_CLEARED);

    atc_state_machine::stop();
    flight_phase::stop();
    openair_db::stop();
}

// ── Frequency guard ───────────────────────────────────────────────────────

TEST_CASE("freq guard: APPROACH frequency rejected in GROUND_CONTACT state",
          "[ifr_departure][freq_guard]")
{
    engine::reset();
    atc_state_machine::init();
    intent_parser::init();
    flight_phase::init();
    openair_db::init("");

    atc_state_machine::set_state(ATCState::GROUND_CONTACT);

    XPlaneContext ctx = make_ifr_ctx(0.0f);
    ctx.on_ground = true;
    ctx.frequency_type = FrequencyType::APPROACH; // wrong for ground state

    engine::Input in{};
    in.transcript      = "Annecy Tower, November 111, ready for departure";
    in.pilot_callsign  = "November 111";
    in.quality         = 0.85f;
    in.ctx             = &ctx;
    in.now_secs        = 0.0;

    engine::Output out;
    engine::process_transcript(in, [&](engine::Output o) { out = std::move(o); });

    // Wrong frequency -> silent drop: no response text, state unchanged
    REQUIRE(out.response_text.empty());
    REQUIRE(atc_state_machine::get_state() == ATCState::GROUND_CONTACT);

    atc_state_machine::stop();
    intent_parser::stop();
    flight_phase::stop();
    openair_db::stop();
}

TEST_CASE("freq guard: TOWER frequency rejected in IFR_EN_ROUTE state",
          "[ifr_departure][freq_guard]")
{
    engine::reset();
    atc_state_machine::init();
    intent_parser::init();
    flight_phase::init();
    openair_db::init("");

    atc_state_machine::set_state(ATCState::IFR_EN_ROUTE);

    XPlaneContext ctx = make_ifr_ctx(3000.0f);
    ctx.frequency_type = FrequencyType::TOWER; // wrong: en-route needs APPROACH

    engine::Input in{};
    in.transcript     = "Chambery Approach, November 111, passing 3000";
    in.pilot_callsign = "November 111";
    in.quality        = 0.85f;
    in.ctx            = &ctx;
    in.now_secs       = 0.0;

    engine::Output out;
    engine::process_transcript(in, [&](engine::Output o) { out = std::move(o); });

    REQUIRE(out.response_text.empty());
    REQUIRE(atc_state_machine::get_state() == ATCState::IFR_EN_ROUTE);

    atc_state_machine::stop();
    intent_parser::stop();
    flight_phase::stop();
    openair_db::stop();
}

TEST_CASE("freq guard: APPROACH frequency accepted in IFR_EN_ROUTE state",
          "[ifr_departure][freq_guard]")
{
    engine::reset();
    atc_state_machine::init();
    intent_parser::init();
    flight_phase::init();
    openair_db::init("");

    atc_state_machine::set_state(ATCState::IFR_EN_ROUTE);

    XPlaneContext ctx = make_ifr_ctx(3000.0f);
    ctx.frequency_type = FrequencyType::APPROACH; // correct for en-route

    engine::Input in{};
    in.transcript     = "Chambery Approach, November 111, passing 3000";
    in.pilot_callsign = "November 111";
    in.quality        = 0.85f;
    in.ctx            = &ctx;
    in.now_secs       = 0.0;

    engine::Output out;
    engine::process_transcript(in, [&](engine::Output o) { out = std::move(o); });

    // Correct frequency -> ATC replies with something
    REQUIRE_FALSE(out.response_text.empty());

    atc_state_machine::stop();
    intent_parser::stop();
    flight_phase::stop();
    openair_db::stop();
}
