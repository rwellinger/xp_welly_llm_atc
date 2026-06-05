/*
 * xp_wellys_atc - headless CLI
 *
 * Minimal settings::* implementations so the engine OBJECT library
 * links in the CLI without pulling in the plugin-only settings.cpp
 * (which depends on XPLMGetPluginInfo to resolve the data dir). Only
 * the getters engine code actually calls are defined here; setters
 * and other entry points are omitted.
 */

#include "persistence/settings.hpp"

#include <cstdlib>
#include <string>

namespace settings {

static std::string env_or(const char *key, const std::string &fallback) {
  const char *v = std::getenv(key);
  return v ? std::string(v) : fallback;
}

// ── Getters used by engine modules ───────────────────────────────

bool debug_logging() { return std::getenv("XP_ATC_DEBUG") != nullptr; }

// Overridable at runtime so scenarios / REPL `set callsign` feed the
// value used by the intent parser (which matches the transcript against
// the configured pilot callsign).
static std::string g_pilot_callsign =
    env_or("XP_ATC_CALLSIGN", "November One Two Three Alpha Bravo");

std::string pilot_callsign() { return g_pilot_callsign; }

void set_pilot_callsign_raw(const std::string &v) { g_pilot_callsign = v; }

std::string pattern_direction() { return "left"; }
float auto_correction_factor() { return 1.0f; }

// Overridable at runtime (scenario-level profile, REPL `set region`).
// Initialised from XP_ATC_REGION env var; defaults to EU. Env var name
// kept for back-compat with existing scenario fixtures - the user-facing
// concept is the ATC profile.
static std::string g_atc_profile = env_or("XP_ATC_REGION", "EU");

std::string atc_profile() { return g_atc_profile; }

std::string backend_language() {
  return atc_profile() == "DE" ? "de" : "en";
}

void set_atc_profile(const std::string &v) {
  std::string up;
  for (char c : v)
    up += (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
  if (up != "EU" && up != "US" && up != "DE") return;
  g_atc_profile = up;
}

std::string get_data_dir() { return env_or("XP_ATC_DATA_DIR", "./data"); }

std::string atc_profile_data_dir() {
  std::string r = atc_profile();
  std::string lower;
  for (char c : r)
    lower += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
  if (lower != "eu" && lower != "us" && lower != "de")
    lower = "eu";
  return get_data_dir() + "/atc_profiles/" + lower;
}

std::string vrps_data_path() {
  return get_data_dir() + "/vrps/airport_vrps.json";
}

// Headless equivalent of the plugin's user_prefs_dir(). The plugin path
// (<X-Plane>/Output/preferences/xp_wellys_atc) does not exist in CLI
// runs; XP_ATC_USER_PREFS_DIR overrides it for fixture-driven tests,
// otherwise we fall back to a sibling of the data dir so REPL users can
// drop overrides next to the bundle.
std::string user_prefs_dir() {
  return env_or("XP_ATC_USER_PREFS_DIR", "./user_prefs");
}

// Unused-by-engine but referenced elsewhere during link: defined just in
// case they creep into the engine OBJECT library from a transitive include.
bool skip_radio_power_check() { return true; }

// Headless scenarios always start fresh (IDLE+PARKED), the start_mode
// setting is a UI affordance only.
std::string start_mode() { return "engines_running"; }

// Voice resolver — pulled into the engine library by backends::manager.
// The headless tools never synthesize audio (TTS calls short-circuit on
// `tts_ready() == false`) so this getter is only here to satisfy the
// linker; its return value never reaches Piper.
std::string voice_for_role(model_manifest::VoiceRole role) {
  return model_manifest::default_voice_for(role);
}

void set_voice_for_role(model_manifest::VoiceRole, const std::string &) {}

} // namespace settings
