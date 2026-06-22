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

#ifndef SETTINGS_HPP
#define SETTINGS_HPP

#include "persistence/model_manifest.hpp"

#include <string>

namespace settings {

void init();
void stop();
void save();

// Data directory path (plugin-relative <plugin>/data)
std::string get_data_dir();

// ATC-profile-scoped data directory
// (e.g. <data>/atc_profiles/eu, <data>/atc_profiles/us). The ATC profile
// is a *user-selected ATC style to train against*, not a geographic
// region — the pilot picks US to train FAA phraseology even when flying
// in Europe.
std::string atc_profile_data_dir();

// Global, profile-independent VRP file path (<data>/vrps/airport_vrps.json).
// VRPs are geographic data and don't depend on which ATC style the pilot
// is training.
std::string vrps_data_path();

// User preferences directory — under
// <X-Plane>/Output/preferences/xp_wellys_atc/. Survives plugin re-installs.
// Used for optional per-user data overrides (e.g. airport_vrps_<region>.json
// sourced from Navigraph Charts). Created on first call if absent.
std::string user_prefs_dir();

// Getters
std::string pilot_callsign();
int active_com();
float volume();
bool debug_logging();
std::string pattern_direction();
bool disable_default_atc();
bool skip_radio_power_check();
bool show_phraseology_hints();
float auto_correction_factor();

// Active ATC training profile — "EU" or "US". Drives which
// data/atc_profiles/<code>/*.json bundle is loaded (templates, intent
// rules, phraseology hints, flight rules, UI strings) and therefore
// which phraseology the controller speaks back. NOT tied to the
// pilot's geographic location. A legacy "DE" value (the DE profile now
// lives in a dedicated plugin) degrades to "EU".
std::string atc_profile();

// ISO-639-1 language code for inference. Always "en" — used by the
// OpenAI backends as the Whisper `language` parameter and to select the
// LM prompt variant in atc_prompt_templates.json.
std::string backend_language();
// Cockpit start state assumed at plugin boot. Drives the initial
// ATCState the state machine adopts. One of:
//   "cold_and_dark"     — IDLE, pilot expected to power up + tune freq
//   "engines_running"   — IDLE, pilot ready to call Ground (default)
//   "ready_for_takeoff" — TOWER_CONTACT, pilot at holding point
std::string start_mode();
bool debug_traffic();

// Debug helper: replace the mic / PTT path with a typed text field in
// the Status tab. When true, the UI shows an InputText below the
// transcript that injects the typed string directly into
// engine::process_transcript via atc_session::submit_text — skipping
// STT entirely. The Tower reply still runs through the live LM + TTS
// strategy, so the rest of the pipeline (including audible Tower voice)
// is exercised. Useful on laptops without a headset and for isolating
// ATC-logic bugs from STT misrecognitions. Default false. PTT remains
// functional in parallel.
bool debug_text_input();

// Master switch for the traffic subsystem (Phase 2/3/4 advisories,
// landing sequencing, go-around trigger). Default true — TCAS dataRefs
// exist on every X-Plane install, and any traffic provider (LiveTraffic,
// xPilot, swift, X-IvAp, native AI) fills them. When false, the runtime
// reader returns early with an empty snapshot and every downstream
// consumer (advisor / pattern_flow overlay / poll_go_around) becomes a
// no-op without further code paths.
bool traffic_features_enabled();
void set_traffic_features_enabled(bool v);

// SimBrief pilot ID (numeric, 0 = not configured).
int simbrief_pilot_id();
void set_simbrief_pilot_id(int id);

// Backend selection. Picks the full inference pipeline:
//   "local"   — whisper.cpp + llama.cpp + Piper (Apple Silicon only)
//   "openai"  — Whisper API + Chat Completions + TTS API
//   "mistral" — Voxtral STT + Mistral chat completions + Voxtral TTS
// Never mixed at runtime. Default "local".
std::string backend_mode();

// True when an OpenAI API key was saved to the Keychain. The actual
// key is never persisted to settings.json — only this flag.
bool api_key_saved();

// OpenAI model selection. Defaults match the v1.3.x integration.
std::string openai_stt_model();
std::string openai_lm_model();
std::string openai_tts_model();

// OpenAI TTS voice per role. One of
// alloy / echo / fable / onyx / nova / shimmer.
std::string openai_tts_voice_atis();
std::string openai_tts_voice_tower();
std::string openai_tts_voice_ground();

// True when a Mistral API key was saved to the Keychain. Mirrors
// api_key_saved() for the OpenAI side.
bool mistral_api_key_saved();

// Mistral model selection. Defaults: voxtral-mini-2507 for STT,
// mistral-small-latest for LM, empty for TTS (Voxtral TTS picks the
// default model server-side).
std::string mistral_stt_model();
std::string mistral_lm_model();
std::string mistral_tts_model();

// Mistral TTS voice per role. Free strings — Voxtral TTS preset voice
// ids are not whitelisted client-side, so the user can paste any
// custom voice id from the Mistral dashboard.
std::string mistral_tts_voice_atis();
std::string mistral_tts_voice_tower();
std::string mistral_tts_voice_ground();

// Setters for the dual-backend settings (used by the Settings UI tab).
void set_backend_mode(const std::string &v);
void set_openai_stt_model(const std::string &v);
void set_openai_lm_model(const std::string &v);
void set_openai_tts_model(const std::string &v);
void set_openai_tts_voice_atis(const std::string &v);
void set_openai_tts_voice_tower(const std::string &v);
void set_openai_tts_voice_ground(const std::string &v);

void set_mistral_stt_model(const std::string &v);
void set_mistral_lm_model(const std::string &v);
void set_mistral_tts_model(const std::string &v);
void set_mistral_tts_voice_atis(const std::string &v);
void set_mistral_tts_voice_tower(const std::string &v);
void set_mistral_tts_voice_ground(const std::string &v);

// Keychain-backed OpenAI API key handling. save_api_key() also
// updates the api_key_saved flag and persists settings.json.
// load_api_key() returns an empty string when no key is stored.
// delete_api_key() clears both the Keychain entry and the flag.
bool save_api_key(const std::string &key);
std::string load_api_key();
void delete_api_key();

// Mistral-side mirror of the OpenAI key helpers above. Uses a
// separate Keychain entry (service "com.xp_wellys_atc.mistral") so
// both keys can persist in parallel and a Backend Mode flip does not
// require re-pasting either one.
bool save_mistral_api_key(const std::string &key);
std::string load_mistral_api_key();
void delete_mistral_api_key();

// Setters
std::string pilot_callsign_raw();
void set_pilot_callsign_raw(const std::string &raw);
std::string to_icao_phonetic(const std::string &raw);
void set_volume(float v);
void set_debug_logging(bool v);
void set_active_com(int com);
void set_pattern_direction(const std::string &v);
void set_disable_default_atc(bool v);
void set_skip_radio_power_check(bool v);
void set_show_phraseology_hints(bool v);
void set_auto_correction_factor(float v);

// Set the ATC training profile. Writes BOTH the canonical "atc_profile"
// key AND the legacy "flow_region" key into settings.json with the
// same value, so a user who rolls back to a pre-rename plugin version
// keeps their profile choice. The legacy key will be removed in a
// later release once rollback is no longer plausible.
void set_atc_profile(const std::string &v);

void set_debug_traffic(bool v);
void set_debug_text_input(bool v);
void set_start_mode(const std::string &v);

// Voice id (Piper voice_id, e.g. "en_US-lessac-medium") currently
// assigned to a logical ATC role. Defaults to the manifest default if
// the setting is missing or points at an unknown voice id.
std::string voice_for_role(model_manifest::VoiceRole role);
void set_voice_for_role(model_manifest::VoiceRole role,
                        const std::string &voice_id);

// Window geometry (-1 = use default/center)
float window_x();
float window_y();
float window_w();
float window_h();
void set_window_geometry(float x, float y, float w, float h);
void reset_window_geometry();

} // namespace settings

#endif // SETTINGS_HPP
