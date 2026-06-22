#include "persistence/model_manifest.hpp"

#include <catch2/catch_amalgamated.hpp>

using model_manifest::Kind;
using model_manifest::VoiceRole;

// ── Language-aware Whisper selection (M6) ────────────────────────────

TEST_CASE("get_for_language picks the EN-only Whisper for 'en'",
          "[model_manifest][m6]") {
    const auto &e = model_manifest::get_for_language(Kind::WhisperModel, "en");
    REQUIRE(e.kind == Kind::WhisperModel);
    REQUIRE(e.language == "en");
    REQUIRE(e.filename == "ggml-small.en-q5_1.bin");
}

TEST_CASE("get_for_language falls back to language-agnostic Llama",
          "[model_manifest][m6]") {
    // Llama is multilingual — no language tag — so any language should
    // route to the same single entry via the empty-language fallback.
    const auto &en = model_manifest::get_for_language(Kind::LlamaModel, "en");
    const auto &other = model_manifest::get_for_language(Kind::LlamaModel, "fr");
    REQUIRE(en.kind == Kind::LlamaModel);
    REQUIRE(other.kind == Kind::LlamaModel);
    REQUIRE(en.filename == other.filename);
    REQUIRE(en.language.empty());
}

// ── Voice catalog language tagging (M6) ──────────────────────────────

TEST_CASE("English voices are tagged 'en'", "[model_manifest][m6]") {
    REQUIRE(model_manifest::voice_language("en_US-lessac-medium") == "en");
    REQUIRE(model_manifest::voice_language("en_GB-alan-medium") == "en");
}

TEST_CASE("voice_language returns empty for unknown ids",
          "[model_manifest][m6]") {
    REQUIRE(model_manifest::voice_language("does-not-exist").empty());
}

// ── default_voice_for(role, language) (M6) ───────────────────────────

TEST_CASE("English defaults stay per-role", "[model_manifest][m6]") {
    REQUIRE(model_manifest::default_voice_for(VoiceRole::Atis, "en") ==
            "en_US-lessac-medium");
    REQUIRE(model_manifest::default_voice_for(VoiceRole::Tower, "en") ==
            "en_US-ryan-high");
    REQUIRE(model_manifest::default_voice_for(VoiceRole::Ground, "en") ==
            "en_US-amy-medium");
    REQUIRE(model_manifest::default_voice_for(VoiceRole::Center, "en") ==
            "en_GB-alan-medium");
}
