/*
 * xp_wellys_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "persistence/settings.hpp"

#include "atc/de_phraseology.hpp"
#include "persistence/keychain.hpp"
#include "persistence/models_catalog.hpp"

#include <cctype>
#include <fstream>
#include <string>
#include <sys/stat.h>

#include <XPLMPlugin.h>
#include <XPLMUtilities.h>
#include <json.hpp>

namespace settings {

using json = nlohmann::json;

static json cfg;
static std::string data_dir_path;

static json default_config() {
  using model_manifest::VoiceRole;
  return {
      {"pilot_callsign_raw", ""},
      {"pilot_callsign", ""},
      {"active_com", 1},
      {"volume", 1.0},
      {"debug_logging", false},
      {"pattern_direction", "left"},
      {"disable_default_atc", false},
      {"skip_radio_power_check", false},
      {"show_phraseology_hints", true},
      {"auto_correction_factor", 1.0},
      // atc_profile is the canonical key as of the region->atc-profile
      // rename. flow_region is written in parallel with the same value
      // for rollback safety (an older binary will read flow_region and
      // keep the user's profile). Drop flow_region in a later release
      // when nobody plausibly rolls back to a pre-rename binary.
      {"atc_profile", "EU"},
      {"flow_region", "EU"},
      {"debug_traffic", false},
      {"debug_text_input", false},
      {"traffic_features_enabled", true},
      {"bzf_strict_mode", false},
      {"start_mode", "engines_running"},
      {"backend_mode", "local"},
      {"api_key_saved", false},
      {"openai_stt_model", "whisper-1"},
      {"openai_lm_model", "gpt-4o-mini"},
      {"openai_tts_model", "tts-1"},
      {"openai_tts_voice_atis", "onyx"},
      {"openai_tts_voice_tower", "echo"},
      {"openai_tts_voice_ground", "alloy"},
      {"mistral_api_key_saved", false},
      // Defaults bumped in v3.1 — the dedicated transcribe model
      // produces noticeably better aviation-English transcripts than
      // the multimodal voxtral-mini-2507, and the large LM picks up
      // intent classification edge cases that small misses. Existing
      // installs are migrated in init() below; users who pasted a
      // custom slug keep their choice.
      {"mistral_stt_model", "voxtral-mini-transcribe-2507"},
      {"mistral_lm_model", "mistral-large-latest"},
      {"mistral_tts_model", "voxtral-mini-tts-2603"},
      {"mistral_tts_voice_atis", "gb_oliver_neutral"},
      {"mistral_tts_voice_tower", "en_paul_confident"},
      {"mistral_tts_voice_ground", "en_paul_neutral"},
      {"voice_atis", model_manifest::default_voice_for(VoiceRole::Atis)},
      {"voice_tower", model_manifest::default_voice_for(VoiceRole::Tower)},
      {"voice_ground", model_manifest::default_voice_for(VoiceRole::Ground)},
      {"voice_center", model_manifest::default_voice_for(VoiceRole::Center)},
      {"window_x", -1.0},
      {"window_y", -1.0},
      {"window_w", -1.0},
      {"window_h", -1.0}};
}

// Strip pre-v1.4 keys that previous plugin versions wrote into
// settings.json under different names. We never read them again;
// clearing them keeps the file tidy when the user upgrades.
// Note: "api_key_saved" was on this list in the local-only era —
// it is now a live key again (dual-backend), so it must NOT appear
// here.
static const char *kLegacyKeys[] = {
    "tts_voice",
    "tts_model",
    "whisper_model",
    "gpt_model",
    "gpt_fallback_enabled",
};

void init() {
  // Resolve plugin path to find data/ directory
  // Installed: .../plugins/xp_wellys_atc/mac_x64/xp_wellys_atc.xpl
  // We need to go up 2 levels to reach the plugin root
  char plugin_path_raw[2048] = {};
  XPLMGetPluginInfo(XPLMGetMyID(), nullptr, plugin_path_raw, nullptr, nullptr);

  std::string path_str(plugin_path_raw);
#if defined(__APPLE__)
  // macOS may return an HFS path (colon-separated) — convert to POSIX
  if (path_str.find(':') != std::string::npos &&
      path_str.find('/') == std::string::npos) {
    auto colon = path_str.find(':');
    std::string posix = path_str.substr(colon + 1);
    for (char &c : posix)
      if (c == ':')
        c = '/';
    path_str = "/" + posix;
  }
#endif

  // Strip filename → directory, then strip platform dir (mac_x64/)
  auto pos = path_str.rfind('/');
  if (pos != std::string::npos) {
    pos = path_str.rfind('/', pos - 1);
  }
  if (pos != std::string::npos) {
    data_dir_path = path_str.substr(0, pos) + "/data";
  } else {
    data_dir_path = "data";
  }

  mkdir(data_dir_path.c_str(), 0755);

  // Load the model catalog BEFORE default_config() — default_config()
  // calls model_manifest::default_voice_for(), which now reads from
  // the catalog. A missing/broken file degrades to the built-in
  // defaults rather than failing init().
  models_catalog::init(data_dir_path);

  std::string json_path = data_dir_path + "/settings.json";
  std::ifstream in(json_path);
  bool needs_save = false;
  if (in.good()) {
    try {
      in >> cfg;
      // Merge any missing defaults
      json defaults = default_config();
      for (auto &[key, value] : defaults.items()) {
        if (!cfg.contains(key)) {
          cfg[key] = value;
        }
      }
    } catch (...) {
      XPLMDebugString("[xp_wellys_atc] Warning: failed to parse settings.json, "
                      "using defaults\n");
      cfg = default_config();
      needs_save = true;
    }
  } else {
    cfg = default_config();
    needs_save = true;
  }

  for (const char *legacy : kLegacyKeys) {
    if (cfg.contains(legacy)) {
      cfg.erase(legacy);
      needs_save = true;
    }
  }

  // Migrate the pre-rename "flow_region" key into the canonical
  // "atc_profile" key. We do NOT delete the legacy key here — leaving
  // it in settings.json keeps an older plugin binary functional if the
  // user rolls back. set_atc_profile() also writes both keys in
  // parallel on every save.
  if (cfg.contains("flow_region")) {
    const std::string fr = cfg.value("flow_region", std::string("EU"));
    if (!cfg.contains("atc_profile")) {
      cfg["atc_profile"] = fr;
      needs_save = true;
    }
  }

  // Mistral-default bump (v3.1): users on the previous hardcoded
  // defaults (voxtral-mini-2507 / mistral-small-latest) get upgraded
  // to the better aviation-English combo. Any user who pasted a
  // different slug keeps it. Non-destructive — match the OLD default
  // exactly, otherwise leave untouched.
  if (cfg.value("mistral_stt_model", std::string{}) == "voxtral-mini-2507") {
    cfg["mistral_stt_model"] = "voxtral-mini-transcribe-2507";
    needs_save = true;
  }
  if (cfg.value("mistral_lm_model", std::string{}) == "mistral-small-latest") {
    cfg["mistral_lm_model"] = "mistral-large-latest";
    needs_save = true;
  }

  if (needs_save)
    save();

  XPLMDebugString("[xp_wellys_atc] Settings loaded\n");
}

void stop() {}

std::string get_data_dir() { return data_dir_path; }

std::string atc_profile_data_dir() {
  std::string profile = atc_profile();
  std::string lower;
  lower.reserve(profile.size());
  for (char c : profile)
    lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (lower != "eu" && lower != "us" && lower != "de")
    lower = "eu";
  return data_dir_path + "/atc_profiles/" + lower;
}

std::string vrps_data_path() {
  return data_dir_path + "/vrps/airport_vrps.json";
}

std::string user_prefs_dir() {
  // XPLMGetSystemPath returns the X-Plane root with a trailing slash.
  // Sits under Output/preferences/ so it survives plugin re-installs.
  char xp_root[2048] = {};
  XPLMGetSystemPath(xp_root);
  std::string path = std::string(xp_root) + "Output/preferences/xp_wellys_atc";
  mkdir(path.c_str(), 0755);
  return path;
}

void save() {
  std::string json_path = data_dir_path + "/settings.json";
  std::ofstream out(json_path);
  if (out.good()) {
    out << cfg.dump(2) << std::endl;
  } else {
    XPLMDebugString("[xp_wellys_atc] Error: failed to write settings.json\n");
  }
}

// --- Getters ---

std::string pilot_callsign_raw() {
  return cfg.value("pilot_callsign_raw", std::string(""));
}
std::string pilot_callsign() {
  // Profile-aware: compute on the fly so an ATC-profile switch flips
  // English-NATO ("Alpha Bravo One") to BZF-German ("Alfa Bravo eins")
  // without needing to re-save the callsign. Falls back to the cached
  // value only when no raw callsign is present (legacy settings.json).
  std::string raw = cfg.value("pilot_callsign_raw", std::string(""));
  if (raw.empty())
    return cfg.value("pilot_callsign", std::string(""));
  if (atc_profile() == "DE")
    return de_phraseology::expand_callsign_phonetic(raw);
  return to_icao_phonetic(raw);
}
int active_com() { return cfg.value("active_com", 1); }
float volume() { return cfg.value("volume", 1.0f); }
bool debug_logging() { return cfg.value("debug_logging", false); }
std::string pattern_direction() {
  return cfg.value("pattern_direction", std::string("left"));
}
bool disable_default_atc() { return cfg.value("disable_default_atc", false); }
bool skip_radio_power_check() {
  return cfg.value("skip_radio_power_check", false);
}
bool show_phraseology_hints() {
  return cfg.value("show_phraseology_hints", true);
}
float auto_correction_factor() {
  return cfg.value("auto_correction_factor", 1.0f);
}
std::string atc_profile() {
  // Prefer the canonical key. Fall back to the legacy "flow_region"
  // key so an older settings.json (pre-rename) still resolves until
  // the next save normalises both keys via set_atc_profile().
  std::string v = cfg.value("atc_profile", std::string{});
  if (v.empty())
    v = cfg.value("flow_region", std::string("EU"));
  if (v == "eu")
    v = "EU";
  else if (v == "us")
    v = "US";
  else if (v == "de")
    v = "DE";
  if (v != "EU" && v != "US" && v != "DE")
    v = "EU";
  return v;
}
std::string backend_language() { return atc_profile() == "DE" ? "de" : "en"; }
bool debug_traffic() { return cfg.value("debug_traffic", false); }
bool debug_text_input() { return cfg.value("debug_text_input", false); }
bool bzf_strict_mode() { return cfg.value("bzf_strict_mode", false); }
bool traffic_features_enabled() {
  return cfg.value("traffic_features_enabled", true);
}
std::string start_mode() {
  std::string v = cfg.value("start_mode", std::string("engines_running"));
  if (v != "cold_and_dark" && v != "engines_running" &&
      v != "ready_for_takeoff")
    v = "engines_running";
  return v;
}

std::string backend_mode() {
  std::string v = cfg.value("backend_mode", std::string("local"));
  if (v != "local" && v != "openai" && v != "mistral")
    v = "local";
  return v;
}

bool api_key_saved() { return cfg.value("api_key_saved", false); }

std::string openai_stt_model() {
  return cfg.value("openai_stt_model", std::string("whisper-1"));
}
std::string openai_lm_model() {
  return cfg.value("openai_lm_model", std::string("gpt-4o-mini"));
}
std::string openai_tts_model() {
  return cfg.value("openai_tts_model", std::string("tts-1"));
}

namespace {
bool is_valid_openai_voice(const std::string &v) {
  return v == "alloy" || v == "echo" || v == "fable" || v == "onyx" ||
         v == "nova" || v == "shimmer";
}
} // namespace

std::string openai_tts_voice_atis() {
  std::string v = cfg.value("openai_tts_voice_atis", std::string("onyx"));
  return is_valid_openai_voice(v) ? v : "onyx";
}
std::string openai_tts_voice_tower() {
  std::string v = cfg.value("openai_tts_voice_tower", std::string("echo"));
  return is_valid_openai_voice(v) ? v : "echo";
}
std::string openai_tts_voice_ground() {
  std::string v = cfg.value("openai_tts_voice_ground", std::string("alloy"));
  return is_valid_openai_voice(v) ? v : "alloy";
}

// --- Setters ---

// ── ICAO phonetic alphabet conversion ───────────────────────────

static const char *phonetic_letter(char c) {
  static const char *letters[] = {
      "Alpha",  "Bravo",   "Charlie", "Delta",  "Echo",   "Foxtrot", "Golf",
      "Hotel",  "India",   "Juliet",  "Kilo",   "Lima",   "Mike",    "November",
      "Oscar",  "Papa",    "Quebec",  "Romeo",  "Sierra", "Tango",   "Uniform",
      "Victor", "Whiskey", "X-Ray",   "Yankee", "Zulu"};
  if (c >= 'A' && c <= 'Z')
    return letters[c - 'A'];
  if (c >= 'a' && c <= 'z')
    return letters[c - 'a'];
  return nullptr;
}

static const char *phonetic_digit(char c) {
  static const char *digits[] = {"Zero", "One", "Two",   "Three", "Four",
                                 "Five", "Six", "Seven", "Eight", "Niner"};
  if (c >= '0' && c <= '9')
    return digits[c - '0'];
  return nullptr;
}

std::string to_icao_phonetic(const std::string &raw) {
  std::string result;
  for (char c : raw) {
    const char *word = phonetic_letter(c);
    if (!word)
      word = phonetic_digit(c);
    if (word) {
      if (!result.empty())
        result += ' ';
      result += word;
    }
    // Skip dashes, spaces, and other non-alphanumeric chars
  }
  return result;
}

void set_pilot_callsign_raw(const std::string &raw) {
  cfg["pilot_callsign_raw"] = raw;
  cfg["pilot_callsign"] = to_icao_phonetic(raw);
}
void set_volume(float v) { cfg["volume"] = v; }
void set_debug_logging(bool v) { cfg["debug_logging"] = v; }
void set_active_com(int com) { cfg["active_com"] = com; }
void set_pattern_direction(const std::string &v) {
  cfg["pattern_direction"] = v;
}
void set_disable_default_atc(bool v) { cfg["disable_default_atc"] = v; }
void set_skip_radio_power_check(bool v) { cfg["skip_radio_power_check"] = v; }
void set_show_phraseology_hints(bool v) { cfg["show_phraseology_hints"] = v; }
void set_auto_correction_factor(float v) {
  if (v < 0.5f)
    v = 0.5f;
  if (v > 2.0f)
    v = 2.0f;
  cfg["auto_correction_factor"] = v;
}
// Forward declaration — defined alongside voice_for_role() below.
static void migrate_voices_for_language();

void set_atc_profile(const std::string &v) {
  std::string prev_lang = backend_language();
  std::string normalized;
  if (v == "US" || v == "us")
    normalized = "US";
  else if (v == "DE" || v == "de")
    normalized = "DE";
  else
    normalized = "EU";
  // Write BOTH the canonical key AND the legacy mirror so a user who
  // rolls back to a pre-rename plugin binary still finds their profile
  // under "flow_region". Remove the legacy mirror in a later release.
  cfg["atc_profile"] = normalized;
  cfg["flow_region"] = normalized;
  if (backend_language() != prev_lang)
    migrate_voices_for_language();
}
void set_debug_traffic(bool v) { cfg["debug_traffic"] = v; }
void set_debug_text_input(bool v) { cfg["debug_text_input"] = v; }
void set_bzf_strict_mode(bool v) { cfg["bzf_strict_mode"] = v; }
void set_traffic_features_enabled(bool v) {
  cfg["traffic_features_enabled"] = v;
}
void set_start_mode(const std::string &v) {
  if (v == "cold_and_dark" || v == "engines_running" ||
      v == "ready_for_takeoff")
    cfg["start_mode"] = v;
  else
    cfg["start_mode"] = "engines_running";
}

// ── Dual-backend settings ─────────────────────────────────────

void set_backend_mode(const std::string &v) {
  if (v == "openai" || v == "mistral")
    cfg["backend_mode"] = v;
  else
    cfg["backend_mode"] = "local";
}
void set_openai_stt_model(const std::string &v) { cfg["openai_stt_model"] = v; }
void set_openai_lm_model(const std::string &v) { cfg["openai_lm_model"] = v; }
void set_openai_tts_model(const std::string &v) { cfg["openai_tts_model"] = v; }
void set_openai_tts_voice_atis(const std::string &v) {
  if (is_valid_openai_voice(v))
    cfg["openai_tts_voice_atis"] = v;
}
void set_openai_tts_voice_tower(const std::string &v) {
  if (is_valid_openai_voice(v))
    cfg["openai_tts_voice_tower"] = v;
}
void set_openai_tts_voice_ground(const std::string &v) {
  if (is_valid_openai_voice(v))
    cfg["openai_tts_voice_ground"] = v;
}

bool save_api_key(const std::string &key) {
  if (!persistence::keychain::save(key)) {
    XPLMDebugString("[xp_wellys_atc] Error: failed to save API key to "
                    "Keychain\n");
    return false;
  }
  cfg["api_key_saved"] = true;
  save();
  return true;
}

std::string load_api_key() { return persistence::keychain::load(); }

void delete_api_key() {
  persistence::keychain::remove();
  cfg["api_key_saved"] = false;
  save();
}

// ── Mistral-side settings + Keychain ──────────────────────────────
//
// Mistral lives in a separate Keychain entry so both providers can
// hold a valid key simultaneously. Switching Backend Mode then never
// needs the user to re-paste a key they already saved earlier.

namespace {
constexpr const char *kMistralKcService = "com.xp_wellys_atc.mistral";
constexpr const char *kMistralKcAccount = "default";
} // namespace

bool mistral_api_key_saved() {
  return cfg.value("mistral_api_key_saved", false);
}

std::string mistral_stt_model() {
  return cfg.value("mistral_stt_model", std::string("voxtral-mini-2507"));
}
std::string mistral_lm_model() {
  return cfg.value("mistral_lm_model", std::string("mistral-small-latest"));
}
std::string mistral_tts_model() {
  // Empty-string fallback heals settings.json files written by the
  // pre-dropdown Mistral build that left the TTS model blank.
  std::string v = cfg.value("mistral_tts_model", std::string(""));
  return v.empty() ? std::string("voxtral-mini-tts-2603") : v;
}
std::string mistral_tts_voice_atis() {
  // Empty-string fallback so users on the pre-dropdown defaults still
  // get a usable voice — the preset catalog only became known after
  // the first Mistral rollout. Cleared settings auto-heal to ATC-best
  // defaults on next read.
  std::string v = cfg.value("mistral_tts_voice_atis", std::string(""));
  return v.empty() ? std::string("gb_oliver_neutral") : v;
}
std::string mistral_tts_voice_tower() {
  std::string v = cfg.value("mistral_tts_voice_tower", std::string(""));
  return v.empty() ? std::string("en_paul_confident") : v;
}
std::string mistral_tts_voice_ground() {
  std::string v = cfg.value("mistral_tts_voice_ground", std::string(""));
  return v.empty() ? std::string("en_paul_neutral") : v;
}

void set_mistral_stt_model(const std::string &v) {
  cfg["mistral_stt_model"] = v;
}
void set_mistral_lm_model(const std::string &v) { cfg["mistral_lm_model"] = v; }
void set_mistral_tts_model(const std::string &v) {
  cfg["mistral_tts_model"] = v;
}
void set_mistral_tts_voice_atis(const std::string &v) {
  cfg["mistral_tts_voice_atis"] = v;
}
void set_mistral_tts_voice_tower(const std::string &v) {
  cfg["mistral_tts_voice_tower"] = v;
}
void set_mistral_tts_voice_ground(const std::string &v) {
  cfg["mistral_tts_voice_ground"] = v;
}

bool save_mistral_api_key(const std::string &key) {
  if (!persistence::keychain::save(kMistralKcService, kMistralKcAccount, key)) {
    XPLMDebugString(
        "[xp_wellys_atc] Error: failed to save Mistral API key to Keychain\n");
    return false;
  }
  cfg["mistral_api_key_saved"] = true;
  save();
  return true;
}

std::string load_mistral_api_key() {
  return persistence::keychain::load(kMistralKcService, kMistralKcAccount);
}

void delete_mistral_api_key() {
  persistence::keychain::remove(kMistralKcService, kMistralKcAccount);
  cfg["mistral_api_key_saved"] = false;
  save();
}

// ── Voice assignments ──────────────────────────────────────────

static const char *voice_key(model_manifest::VoiceRole role) {
  using R = model_manifest::VoiceRole;
  switch (role) {
  case R::Atis:
    return "voice_atis";
  case R::Tower:
    return "voice_tower";
  case R::Ground:
    return "voice_ground";
  case R::Center:
    return "voice_center";
  }
  return "voice_atis";
}

// Validate a stored voice id against the current manifest. If the
// settings file refers to a voice we no longer ship (manifest bumped,
// catalog shrank), fall back to the role's default. Avoids crashing
// the TTS load path on an unknown id.
static bool voice_id_is_known(const std::string &id) {
  const auto &ids = model_manifest::voice_ids();
  for (const auto &v : ids)
    if (v == id)
      return true;
  return false;
}

std::string voice_for_role(model_manifest::VoiceRole role) {
  // Cloud modes read from their own voice slots — the Piper-style
  // model_manifest::voice_ids() are local-only and would be rejected
  // by the cloud TTS has_voice() checks. Center has no dedicated
  // cloud slot in either provider, so it reuses the Tower voice.
  const std::string mode = backend_mode();
  if (mode == "openai") {
    using R = model_manifest::VoiceRole;
    switch (role) {
    case R::Atis:
      return openai_tts_voice_atis();
    case R::Tower:
      return openai_tts_voice_tower();
    case R::Ground:
      return openai_tts_voice_ground();
    case R::Center:
      return openai_tts_voice_tower();
    }
  }
  if (mode == "mistral") {
    using R = model_manifest::VoiceRole;
    switch (role) {
    case R::Atis:
      return mistral_tts_voice_atis();
    case R::Tower:
      return mistral_tts_voice_tower();
    case R::Ground:
      return mistral_tts_voice_ground();
    case R::Center:
      return mistral_tts_voice_tower();
    }
  }
  std::string id = cfg.value(voice_key(role), std::string{});
  if (id.empty() || !voice_id_is_known(id))
    id = model_manifest::default_voice_for(role, backend_language());
  return id;
}

void set_voice_for_role(model_manifest::VoiceRole role,
                        const std::string &voice_id) {
  if (!voice_id_is_known(voice_id))
    return;
  cfg[voice_key(role)] = voice_id;
}

// Rewrite voice_atis/tower/ground/center to a language-matched default
// when the active backend language no longer matches what is stored.
// Called from set_atc_profile() on a real profile change. Keeps cfg in
// sync with the runtime so the UI dropdown and the loader read the
// same voice id.
static void migrate_voices_for_language() {
  const std::string lang = backend_language();
  for (auto role : model_manifest::all_roles()) {
    const std::string cur = cfg.value(voice_key(role), std::string{});
    if (cur.empty() || model_manifest::voice_language(cur) != lang) {
      cfg[voice_key(role)] = model_manifest::default_voice_for(role, lang);
    }
  }
}

float window_x() { return cfg.value("window_x", -1.0f); }
float window_y() { return cfg.value("window_y", -1.0f); }
float window_w() { return cfg.value("window_w", -1.0f); }
float window_h() { return cfg.value("window_h", -1.0f); }
void set_window_geometry(float x, float y, float w, float h) {
  cfg["window_x"] = x;
  cfg["window_y"] = y;
  cfg["window_w"] = w;
  cfg["window_h"] = h;
}
void reset_window_geometry() {
  cfg["window_x"] = -1.0;
  cfg["window_y"] = -1.0;
  cfg["window_w"] = -1.0;
  cfg["window_h"] = -1.0;
  save();
}

} // namespace settings
