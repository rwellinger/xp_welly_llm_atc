// M7 DoD: 20 typical BZF (Beschraenkt Zugeteiltes Sprechfunkzeugnis)
// transmissions from AIP Germany / training material classify
// correctly via the rule parser at high confidence. The reverse
// normalizer (de_phraseology::parse_spoken_number) runs first inside
// intent_parser::parse() so ziffernweise pronunciation ("eins null
// eins drei") is collapsed to raw digits before rule matching.

#include "atc/intent_parser.hpp"
#include "atc/intent_rules.hpp"
#include "core/xplane_context.hpp"
#include "persistence/settings.hpp"

#include <catch2/catch_amalgamated.hpp>

#include <string>

using intent_parser::parse;
using intent_parser::PilotIntent;

namespace {

// RAII guard: switches flow_region to "DE" + sets a German-style
// pilot callsign + forces intent_rules to reload from the DE JSON
// for the duration of the test case, then restores both so
// neighbouring TUs (test_intent_parser.cpp) are not contaminated.
// intent_rules caches the rule table on first parse() call; without
// the explicit reload, EU/US rules loaded by earlier tests would
// stick around and DE tests would mis-classify.
struct DeRegionGuard {
    std::string saved_region;
    std::string saved_callsign;
    DeRegionGuard()
        : saved_region(settings::atc_profile()),
          saved_callsign(settings::pilot_callsign()) {
        settings::set_atc_profile("DE");
        settings::set_pilot_callsign_raw("Delta Echo Whiskey Lima Yankee");
        intent_rules::reload();
    }
    ~DeRegionGuard() {
        settings::set_atc_profile(saved_region);
        settings::set_pilot_callsign_raw(saved_callsign);
        intent_rules::reload();
    }
};

xplane_context::XPlaneContext ground_ctx() {
    xplane_context::XPlaneContext ctx;
    ctx.on_ground = true;
    ctx.is_towered_airport = true;
    return ctx;
}

xplane_context::XPlaneContext airborne_ctx() {
    xplane_context::XPlaneContext ctx;
    ctx.on_ground = false;
    ctx.is_towered_airport = true;
    return ctx;
}

} // namespace

// ── Departure phase (ground) ─────────────────────────────────────────

TEST_CASE("DE: VFR Abflug 'abflugbereit nach Osten' -> READY_FOR_DEPARTURE_VFR",
          "[intent][de][departure]") {
    DeRegionGuard g;
    auto ctx = ground_ctx();
    auto m = parse("Friedrichshafen Turm, Delta Echo Whiskey Lima Yankee, "
                   "am Rollhalt Piste zwo vier, abflugbereit, nach Osten.",
                   ctx);
    REQUIRE(m.intent == PilotIntent::READY_FOR_DEPARTURE_VFR);
    REQUIRE(m.confidence >= 0.85f);
}

TEST_CASE("DE: 'abflugbereit Piste 24' -> READY_FOR_DEPARTURE",
          "[intent][de][departure]") {
    DeRegionGuard g;
    auto ctx = ground_ctx();
    auto m = parse("Delta Echo Whiskey Lima Yankee, am Rollhalt Piste 24, "
                   "abflugbereit.",
                   ctx);
    REQUIRE(m.intent == PilotIntent::READY_FOR_DEPARTURE);
    REQUIRE(m.confidence >= 0.85f);
}

TEST_CASE("DE: Boden initial call -> INITIAL_CALL_GROUND",
          "[intent][de][initial_call]") {
    DeRegionGuard g;
    auto ctx = ground_ctx();
    // Pure initial call: facility ('Boden') + position, no taxi
    // request keyword. With 'rollen' the rule parser correctly
    // prefers REQUEST_TAXI -- see separate test below.
    auto m = parse("Friedrichshafen Boden, Delta Echo Whiskey Lima Yankee, "
                   "an der Tankstelle, Information Alfa.",
                   ctx);
    REQUIRE(m.intent == PilotIntent::INITIAL_CALL_GROUND);
    REQUIRE(m.confidence >= 0.80f);
}

TEST_CASE("DE: Boden + erbitte Rollen -> REQUEST_TAXI",
          "[intent][de][taxi]") {
    DeRegionGuard g;
    auto ctx = ground_ctx();
    auto m = parse("Friedrichshafen Boden, Delta Echo Whiskey Lima Yankee, "
                   "an der Tankstelle, erbitte Rollen zum Rollhalt Piste 24.",
                   ctx);
    REQUIRE(m.intent == PilotIntent::REQUEST_TAXI);
    REQUIRE(m.confidence >= 0.85f);
}

TEST_CASE("DE: Rollfreigabe readback 'rollen Sie ueber Charlie' -> READBACK",
          "[intent][de][readback]") {
    DeRegionGuard g;
    auto ctx = ground_ctx();
    auto m = parse("Delta Echo Whiskey Lima Yankee, rollen Sie zum Rollhalt "
                   "Piste zwo vier ueber Charlie, QNH eins null eins drei.",
                   ctx);
    REQUIRE(m.intent == PilotIntent::READBACK);
    REQUIRE(m.confidence >= 0.85f);
}

TEST_CASE("DE: Funkpruefung -> RADIO_CHECK",
          "[intent][de][radio_check]") {
    DeRegionGuard g;
    auto ctx = ground_ctx();
    auto m = parse("Funkpruefung, Delta Echo Whiskey Lima Yankee.", ctx);
    REQUIRE(m.intent == PilotIntent::RADIO_CHECK);
    REQUIRE(m.confidence >= 0.85f);
}

// "Wiederholen Sie" / "Say again" — pilot asks tower to repeat the
// last clearance (NfL §18 c) Nr. 4). Critical UX for strict-mode:
// when the pilot forgets the QNH and can't read it back, they must
// be able to ask for a replay without falling into the corrective
// loop. Added 2026-06-05 (user EDNY test).
TEST_CASE("DE: 'Wiederholen Sie' -> REQUEST_REPEAT",
          "[intent][de][repeat]") {
    DeRegionGuard g;
    auto ctx = ground_ctx();
    auto m = parse(
        "Wiederholen Sie, Hotel Bravo Delta Sierra Victor.", ctx);
    REQUIRE(m.intent == PilotIntent::REQUEST_REPEAT);
    REQUIRE(m.confidence >= 0.9f);
}

TEST_CASE("DE: 'Sagen Sie nochmals' -> REQUEST_REPEAT",
          "[intent][de][repeat]") {
    DeRegionGuard g;
    auto ctx = ground_ctx();
    auto m = parse("Sagen Sie nochmals.", ctx);
    REQUIRE(m.intent == PilotIntent::REQUEST_REPEAT);
}

TEST_CASE("DE: 'Say again' -> REQUEST_REPEAT (English variant accepted)",
          "[intent][de][repeat]") {
    DeRegionGuard g;
    auto ctx = ground_ctx();
    auto m = parse("Say again, HB-DSV.", ctx);
    REQUIRE(m.intent == PilotIntent::REQUEST_REPEAT);
}

// "Funkprobe" is the colloquial BZF variant of "Funkpruefung". Whisper
// reliably transcribes it that way and pilots in real DACH operation
// say it interchangeably — must classify as RADIO_CHECK and not fall
// through to INITIAL_CALL_TOWER via the facility-keyword "Tower".
// Regression fix from user test 2026-06-05 (EDNY tower_only).
TEST_CASE("DE: Funkprobe -> RADIO_CHECK (not INITIAL_CALL_TOWER)",
          "[intent][de][radio_check]") {
    DeRegionGuard g;
    auto ctx = ground_ctx();
    auto m = parse(
        "Friedrichshafen Tower, Hotel Bravo Delta Sierra Victor, Funkprobe 120,075.",
        ctx);
    REQUIRE(m.intent == PilotIntent::RADIO_CHECK);
    REQUIRE(m.confidence >= 0.85f);
}

// ── Pattern phase (airborne) ─────────────────────────────────────────

TEST_CASE("DE: Im Gegenanflug -> REPORT_POSITION_DOWNWIND",
          "[intent][de][position]") {
    DeRegionGuard g;
    auto ctx = airborne_ctx();
    auto m = parse("Delta Echo Whiskey Lima Yankee, im Gegenanflug Piste 24.",
                   ctx);
    REQUIRE(m.intent == PilotIntent::REPORT_POSITION_DOWNWIND);
    REQUIRE(m.confidence >= 0.85f);
}

TEST_CASE("DE: Im Queranflug -> REPORT_POSITION_BASE",
          "[intent][de][position]") {
    DeRegionGuard g;
    auto ctx = airborne_ctx();
    auto m = parse("Delta Echo Whiskey Lima Yankee, im Queranflug Piste 24.",
                   ctx);
    REQUIRE(m.intent == PilotIntent::REPORT_POSITION_BASE);
    REQUIRE(m.confidence >= 0.85f);
}

TEST_CASE("DE: Im Endanflug -> REPORT_POSITION_FINAL",
          "[intent][de][position]") {
    DeRegionGuard g;
    auto ctx = airborne_ctx();
    auto m = parse("Delta Echo Whiskey Lima Yankee, im Endanflug Piste zwo vier.",
                   ctx);
    REQUIRE(m.intent == PilotIntent::REPORT_POSITION_FINAL);
    REQUIRE(m.confidence >= 0.85f);
}

TEST_CASE("DE: Durchstarten -> GO_AROUND",
          "[intent][de][go_around]") {
    DeRegionGuard g;
    auto ctx = airborne_ctx();
    auto m = parse("Durchstarten, Delta Echo Whiskey Lima Yankee.", ctx);
    REQUIRE(m.intent == PilotIntent::GO_AROUND);
    REQUIRE(m.confidence >= 0.85f);
}

TEST_CASE("DE: Friedrichshafen Turm zur Landung -> INITIAL_CALL_INBOUND",
          "[intent][de][inbound]") {
    DeRegionGuard g;
    auto ctx = airborne_ctx();
    auto m = parse("Friedrichshafen Turm, Delta Echo Whiskey Lima Yankee, "
                   "zur Landung, Piste 24.",
                   ctx);
    REQUIRE(m.intent == PilotIntent::INITIAL_CALL_INBOUND);
    REQUIRE(m.confidence >= 0.80f);
}

TEST_CASE("DE: Anflugkontrolle -> INITIAL_CALL_APPROACH",
          "[intent][de][initial_call]") {
    DeRegionGuard g;
    auto ctx = airborne_ctx();
    auto m = parse("Stuttgart Anflug, Delta Echo Whiskey Lima Yankee, "
                   "VFR zur Landung.",
                   ctx);
    REQUIRE(m.intent == PilotIntent::INITIAL_CALL_APPROACH);
    REQUIRE(m.confidence >= 0.80f);
}

TEST_CASE("DE: Verkehr in Sicht -> TRAFFIC_IN_SIGHT",
          "[intent][de][traffic]") {
    DeRegionGuard g;
    auto ctx = airborne_ctx();
    auto m = parse("Verkehr in Sicht, Delta Echo Whiskey Lima Yankee.", ctx);
    REQUIRE(m.intent == PilotIntent::TRAFFIC_IN_SIGHT);
    REQUIRE(m.confidence >= 0.85f);
}

TEST_CASE("DE: Negativer Kontakt -> TRAFFIC_NEGATIVE_CONTACT",
          "[intent][de][traffic]") {
    DeRegionGuard g;
    auto ctx = airborne_ctx();
    auto m = parse("Negativer Kontakt, Delta Echo Whiskey Lima Yankee.", ctx);
    REQUIRE(m.intent == PilotIntent::TRAFFIC_NEGATIVE_CONTACT);
    REQUIRE(m.confidence >= 0.85f);
}

TEST_CASE("DE: Halte Ausschau -> TRAFFIC_LOOKING",
          "[intent][de][traffic]") {
    DeRegionGuard g;
    auto ctx = airborne_ctx();
    auto m = parse("Halte Ausschau, Delta Echo Whiskey Lima Yankee.", ctx);
    REQUIRE(m.intent == PilotIntent::TRAFFIC_LOOKING);
}

// ── Landing phase ────────────────────────────────────────────────────

TEST_CASE("DE: Piste frei -> RUNWAY_VACATED",
          "[intent][de][vacated]") {
    DeRegionGuard g;
    auto ctx = ground_ctx();
    auto m = parse("Delta Echo Whiskey Lima Yankee, Piste frei.", ctx);
    REQUIRE(m.intent == PilotIntent::RUNWAY_VACATED);
    REQUIRE(m.confidence >= 0.85f);
}

TEST_CASE("DE: Piste verlassen -> RUNWAY_VACATED",
          "[intent][de][vacated]") {
    DeRegionGuard g;
    auto ctx = ground_ctx();
    auto m = parse("Delta Echo Whiskey Lima Yankee, habe die Piste verlassen.",
                   ctx);
    REQUIRE(m.intent == PilotIntent::RUNWAY_VACATED);
    REQUIRE(m.confidence >= 0.85f);
}

// ── Readback / control flow ──────────────────────────────────────────

TEST_CASE("DE: Verstanden -> READBACK",
          "[intent][de][readback]") {
    DeRegionGuard g;
    auto ctx = ground_ctx();
    auto m = parse("Verstanden, Delta Echo Whiskey Lima Yankee.", ctx);
    REQUIRE(m.intent == PilotIntent::READBACK);
    REQUIRE(m.confidence >= 0.85f);
}

TEST_CASE("DE: QNH readback with ziffernweise -> READBACK",
          "[intent][de][readback][bzf]") {
    DeRegionGuard g;
    auto ctx = ground_ctx();
    auto m = parse(
        "Information Alfa, QNH eins null eins drei, Delta Echo Whiskey Lima Yankee.",
        ctx);
    REQUIRE(m.intent == PilotIntent::READBACK);
}

TEST_CASE("DE: Frequenzwechsel announcement -> LEAVING_FREQUENCY",
          "[intent][de][frequency]") {
    DeRegionGuard g;
    auto ctx = airborne_ctx();
    auto m = parse(
        "Frequenzwechsel zu eins eins acht Komma drei null null, "
        "Delta Echo Whiskey Lima Yankee.",
        ctx);
    REQUIRE(m.intent == PilotIntent::LEAVING_FREQUENCY);
    REQUIRE(m.confidence >= 0.80f);
}

TEST_CASE("DE: Negativ correction -> NEGATIVE_CORRECTION",
          "[intent][de][correction]") {
    DeRegionGuard g;
    auto ctx = ground_ctx();
    auto m = parse("Negativ, Delta Echo Whiskey Lima Yankee, "
                   "Piste 25 war falsch.",
                   ctx);
    REQUIRE(m.intent == PilotIntent::NEGATIVE_CORRECTION);
    REQUIRE(m.confidence >= 0.85f);
}

// ── Feature extractors (runway + callsign) ───────────────────────────

TEST_CASE("DE: extract_runway from 'Piste zwo fuenf links'",
          "[intent][de][runway]") {
    DeRegionGuard g;
    auto ctx = ground_ctx();
    auto m = parse("Delta Echo Whiskey Lima Yankee, am Rollhalt Piste zwo fuenf "
                   "links, abflugbereit.",
                   ctx);
    REQUIRE(m.runway == "25L");
}

TEST_CASE("DE: extract_callsign from D-EWLY spoken sequence",
          "[intent][de][callsign]") {
    DeRegionGuard g;
    auto ctx = ground_ctx();
    auto m = parse("Friedrichshafen Boden, Delta Echo Whiskey Lima Yankee, "
                   "an der Tankstelle, erbitte Rollen.",
                   ctx);
    REQUIRE(m.callsign == "Delta Echo Whiskey Lima Yankee");
}

// ── DoD aggregate check ──────────────────────────────────────────────
// Sanity that the parser does NOT regress to LM-bound UNKNOWN on the
// 20 BZF cases above; Catch2 counts each TEST_CASE individually, so
// the DoD ">= 70%" is satisfied by having >= 14 of the high-confidence
// tests above pass. This aggregate test_case is a backstop that
// double-checks the most discriminating phrases are not UNKNOWN.
TEST_CASE("DE: aggregate sanity -- core BZF phrases never classify as UNKNOWN",
          "[intent][de][aggregate]") {
    DeRegionGuard g;
    auto ctx_g = ground_ctx();
    auto ctx_a = airborne_ctx();

    REQUIRE(parse("Funkpruefung", ctx_g).intent != PilotIntent::UNKNOWN);
    REQUIRE(parse("Durchstarten", ctx_a).intent != PilotIntent::UNKNOWN);
    REQUIRE(parse("Verstanden", ctx_g).intent != PilotIntent::UNKNOWN);
    REQUIRE(parse("im Gegenanflug", ctx_a).intent != PilotIntent::UNKNOWN);
    REQUIRE(parse("im Queranflug", ctx_a).intent != PilotIntent::UNKNOWN);
    REQUIRE(parse("im Endanflug", ctx_a).intent != PilotIntent::UNKNOWN);
    REQUIRE(parse("Piste frei", ctx_g).intent != PilotIntent::UNKNOWN);
    REQUIRE(parse("Verkehr in Sicht", ctx_a).intent != PilotIntent::UNKNOWN);
    REQUIRE(parse("abflugbereit Piste 24", ctx_g).intent != PilotIntent::UNKNOWN);
    REQUIRE(parse("Friedrichshafen Turm", ctx_g).intent != PilotIntent::UNKNOWN);
}
