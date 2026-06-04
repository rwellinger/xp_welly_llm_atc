#include "atc/de_phraseology.hpp"

#include <catch2/catch_amalgamated.hpp>

#include <fstream>
#include <sstream>
#include <string>

using de_phraseology::normalize_for_speech;

// ── Pisten ───────────────────────────────────────────────────────────

TEST_CASE("Piste: two-digit runway", "[de_phraseology][runway]") {
    REQUIRE(normalize_for_speech("Piste 25") == "Piste zwo fuenf");
}

TEST_CASE("Piste: leading-zero runway", "[de_phraseology][runway]") {
    REQUIRE(normalize_for_speech("Piste 07") == "Piste null sieben");
}

TEST_CASE("Piste: suffix L translates to links", "[de_phraseology][runway]") {
    REQUIRE(normalize_for_speech("Piste 36L") == "Piste drei sechs links");
}

TEST_CASE("Piste: suffix C translates to mitte", "[de_phraseology][runway]") {
    REQUIRE(normalize_for_speech("Piste 14C") == "Piste eins vier mitte");
}

TEST_CASE("Piste: already-German suffix stays intact",
          "[de_phraseology][runway]") {
    REQUIRE(normalize_for_speech("Piste 25 links") ==
            "Piste zwo fuenf links");
}

// ── QNH ──────────────────────────────────────────────────────────────

TEST_CASE("QNH: standard 4-digit value", "[de_phraseology][qnh]") {
    REQUIRE(normalize_for_speech("QNH 1013") ==
            "QNH eins null eins drei Hektopascal");
}

TEST_CASE("QNH: non-standard value 1020", "[de_phraseology][qnh]") {
    REQUIRE(normalize_for_speech("QNH 1020") ==
            "QNH eins null zwo null Hektopascal");
}

TEST_CASE("QNH: pre-existing Hektopascal not duplicated",
          "[de_phraseology][qnh][idempotent]") {
    REQUIRE(normalize_for_speech("QNH 1013 Hektopascal") ==
            "QNH eins null eins drei Hektopascal");
}

// ── Frequenzen ───────────────────────────────────────────────────────

TEST_CASE("Frequency: 118.300", "[de_phraseology][frequency]") {
    REQUIRE(normalize_for_speech("Frequenz 118.300") ==
            "Frequenz eins eins acht Komma drei null null");
}

TEST_CASE("Frequency: 119.450", "[de_phraseology][frequency]") {
    REQUIRE(normalize_for_speech("119.450") ==
            "eins eins neun Komma vier fuenf null");
}

TEST_CASE("Frequency: 120.075", "[de_phraseology][frequency]") {
    REQUIRE(normalize_for_speech("auf 120.075") ==
            "auf eins zwo null Komma null sieben fuenf");
}

// ── Steuerkurse ──────────────────────────────────────────────────────

TEST_CASE("Steuerkurs: 050 (already 3-digit)", "[de_phraseology][heading]") {
    REQUIRE(normalize_for_speech("Steuerkurs 050") ==
            "Steuerkurs null fuenf null");
}

TEST_CASE("Steuerkurs: 180", "[de_phraseology][heading]") {
    REQUIRE(normalize_for_speech("Steuerkurs 180") ==
            "Steuerkurs eins acht null");
}

TEST_CASE("Steuerkurs: 360 full circle", "[de_phraseology][heading]") {
    REQUIRE(normalize_for_speech("Steuerkurs 360") ==
            "Steuerkurs drei sechs null");
}

// ── Callsigns ────────────────────────────────────────────────────────

TEST_CASE("Callsign: D-EXYZ expands fully NATO",
          "[de_phraseology][callsign]") {
    REQUIRE(normalize_for_speech("D-EXYZ") ==
            "Delta Echo X-Ray Yankee Zulu");
}

TEST_CASE("Callsign: D-EWLY", "[de_phraseology][callsign]") {
    REQUIRE(normalize_for_speech("D-EWLY") ==
            "Delta Echo Whiskey Lima Yankee");
}

// ── NATO swap ────────────────────────────────────────────────────────

TEST_CASE("Information letter: Alpha becomes Alfa",
          "[de_phraseology][nato]") {
    REQUIRE(normalize_for_speech("Information Alpha") ==
            "Information Alfa");
}

// ── Hoehen ───────────────────────────────────────────────────────────

TEST_CASE("Altitude: 3500 Fuss uses tausend/hundert",
          "[de_phraseology][altitude]") {
    REQUIRE(normalize_for_speech("3500 Fuss") ==
            "drei tausend fuenfhundert Fuss");
}

TEST_CASE("Altitude: 1000 Fuss is einfach tausend",
          "[de_phraseology][altitude]") {
    REQUIRE(normalize_for_speech("1000 Fuss") == "eins tausend Fuss");
}

TEST_CASE("Altitude: 2000 Fuss uses zwo tausend",
          "[de_phraseology][altitude]") {
    REQUIRE(normalize_for_speech("2000 Fuss") == "zwo tausend Fuss");
}

TEST_CASE("Altitude: below 1000 falls back to ziffernweise",
          "[de_phraseology][altitude]") {
    REQUIRE(normalize_for_speech("500 Fuss") == "fuenf null null Fuss");
}

// ── Wind ─────────────────────────────────────────────────────────────

TEST_CASE("Wind: 250 Grad 15 Knoten", "[de_phraseology][wind]") {
    REQUIRE(normalize_for_speech("Wind 250 Grad 15 Knoten") ==
            "Wind zwo fuenf null Grad eins fuenf Knoten");
}

TEST_CASE("Wind: 240 Grad 8 Knoten (single-digit speed)",
          "[de_phraseology][wind]") {
    REQUIRE(normalize_for_speech("Wind 240 Grad 8 Knoten") ==
            "Wind zwo vier null Grad acht Knoten");
}

// ── Clock / Nummer ───────────────────────────────────────────────────

TEST_CASE("Clock: 2 Uhr", "[de_phraseology][clock]") {
    REQUIRE(normalize_for_speech("Verkehr 2 Uhr") == "Verkehr zwo Uhr");
}

TEST_CASE("Sequence number", "[de_phraseology][sequence]") {
    REQUIRE(normalize_for_speech("Nummer 2") == "Nummer zwo");
}

// ── Composite (M2 Worked Examples) ───────────────────────────────────

TEST_CASE("Composite: initial-call-ground full template",
          "[de_phraseology][composite]") {
    const std::string in =
        "D-EWLY, Friedrichshafen Turm, rollen Sie zum Rollhalt Piste 24 "
        "ueber Charlie, QNH 1018.";
    const std::string want =
        "Delta Echo Whiskey Lima Yankee, Friedrichshafen Turm, rollen "
        "Sie zum Rollhalt Piste zwo vier ueber Charlie, QNH eins null "
        "eins acht Hektopascal.";
    REQUIRE(normalize_for_speech(in) == want);
}

TEST_CASE("Composite: takeoff clearance", "[de_phraseology][composite]") {
    const std::string in =
        "D-EWLY, Wind 240 Grad 8 Knoten, Piste 24, Startfreigabe.";
    const std::string want =
        "Delta Echo Whiskey Lima Yankee, Wind zwo vier null Grad acht "
        "Knoten, Piste zwo vier, Startfreigabe.";
    REQUIRE(normalize_for_speech(in) == want);
}

TEST_CASE("Composite: landing clearance", "[de_phraseology][composite]") {
    const std::string in =
        "D-EWLY, Wind 240 Grad 8 Knoten, Piste 24, Landefreigabe.";
    const std::string want =
        "Delta Echo Whiskey Lima Yankee, Wind zwo vier null Grad acht "
        "Knoten, Piste zwo vier, Landefreigabe.";
    REQUIRE(normalize_for_speech(in) == want);
}

// ── Idempotency ──────────────────────────────────────────────────────

TEST_CASE("Idempotency: applying twice yields same output",
          "[de_phraseology][idempotent]") {
    const std::string in =
        "D-EWLY, Piste 24, QNH 1013, Wind 250 Grad 15 Knoten, "
        "Information Alpha, 3500 Fuss, Steuerkurs 050, 119.450.";
    const std::string once = normalize_for_speech(in);
    const std::string twice = normalize_for_speech(once);
    REQUIRE(once == twice);
}

// ── Edge cases ───────────────────────────────────────────────────────

TEST_CASE("Edge: empty string", "[de_phraseology][edge]") {
    REQUIRE(normalize_for_speech("") == "");
}

TEST_CASE("Edge: text without aviation context untouched",
          "[de_phraseology][edge]") {
    const std::string in = "guten Tag, Frequenzwechsel genehmigt.";
    REQUIRE(normalize_for_speech(in) == in);
}

// ── Template invariant ───────────────────────────────────────────────

// Templates must keep raw-digit placeholders ({runway}, {qnh}) so that
// the normalizer is the single source of ziffernweise expansion. If a
// template ever ships with literal "zwo "/"null sieben"/"fuenf"
// fragments, double-normalization risk is back on the table and the
// reverse-normalizer (M7) has nothing stable to map.
TEST_CASE("DE templates contain no pre-expanded BZF digit words",
          "[de_phraseology][templates][invariant]") {
    const std::string path =
        std::string(XP_WELLYS_ATC_SOURCE_DIR) +
        "/data/regions/de/atc_templates.json";
    std::ifstream f(path);
    REQUIRE(f.is_open());
    std::stringstream buf;
    buf << f.rdbuf();
    const std::string body = buf.str();

    // Per-token guards: every match indicates a template author wrote
    // out the BZF spelling instead of leaving the placeholder for the
    // normalizer. (Note: "eins" appears legitimately in standalone
    // phrases like "Nummer eins"; we deliberately do not assert
    // against that token.)
    REQUIRE(body.find("zwo ") == std::string::npos);
    REQUIRE(body.find("null sieben") == std::string::npos);
    REQUIRE(body.find("fuenf ") == std::string::npos);
    REQUIRE(body.find("Hektopascal") == std::string::npos);
}
