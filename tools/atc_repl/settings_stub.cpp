/*
 * xp_wellys_atc - headless CLI
 *
 * Minimal settings::* implementations so the engine OBJECT library links
 * in the CLI without pulling in the plugin-only settings.cpp (Keychain,
 * XPLMGetPluginInfo, etc.). Only the getters engine code actually calls
 * are defined here; setters and other entry points are omitted.
 */

#include "settings.hpp"

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

// Overridable at runtime (scenario-level region, REPL `set region`).
// Initialised from XP_ATC_REGION env var; defaults to EU.
static std::string g_flow_region = env_or("XP_ATC_REGION", "EU");

std::string flow_region() { return g_flow_region; }

void set_flow_region(const std::string &v) {
  std::string up;
  for (char c : v)
    up += (c >= 'a' && c <= 'z') ? static_cast<char>(c - 'a' + 'A') : c;
  if (up != "EU" && up != "US") return;
  g_flow_region = up;
}

std::string get_data_dir() { return env_or("XP_ATC_DATA_DIR", "./data"); }

std::string region_data_dir() {
  std::string r = flow_region();
  std::string lower;
  for (char c : r)
    lower += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
  if (lower != "eu" && lower != "us")
    lower = "eu";
  return get_data_dir() + "/regions/" + lower;
}

std::string tts_voice_tower() { return "onyx"; }
std::string tts_model() { return "tts-1"; }
std::string gpt_model() { return "gpt-4o-mini"; }

std::string get_api_key() { return env_or("OPENAI_API_KEY", ""); }

// Unused-by-engine but referenced elsewhere during link: defined just in
// case they creep into the engine OBJECT library from a transitive include.
bool skip_radio_power_check() { return true; }

} // namespace settings
