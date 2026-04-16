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

#include "atc_ui.hpp"
#include "airport_vrps.hpp"
#include "airspace_db.hpp"
#include "atc_session.hpp"
#include "atc_state_machine.hpp"
#include "atc_templates.hpp"
#include "atis_generator.hpp"
#include "audio_player.hpp"
#include "audio_recorder.hpp"
#include "flight_phase.hpp"
#include "intent_parser.hpp"
#include "settings.hpp"
#include "xplane_context.hpp"

#include <XPLMDataAccess.h>
#include <XPLMDisplay.h>
#include <XPLMGraphics.h>
#include <XPLMUtilities.h>

#include <imgui.h>
#include <imgui_impl_opengl2.h>

#if defined(__APPLE__)
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace atc_ui {

// ── State ────────────────────────────────────────────────────────
// The XPLM window is full-screen and invisible (DecorationNone).
// It exists only to capture mouse/keyboard events and feed them to ImGui.
// ImGui draws its own window on top.
static XPLMWindowID window_id = nullptr;
static bool visible = false;
static bool atc_panel_visible_ = false;

// ImGui persistent buffers
static char api_key_buf[256] = {};
static char callsign_raw_buf[64] = {};
static float save_feedback_timer = 0.0f;
static bool key_just_saved = false;
static bool buffers_initialized = false;

static const char *voice_names[] = {"alloy", "echo", "fable",
                                    "onyx",  "nova", "shimmer"};
static const int voice_count = 6;
static int voice_sel_atis = 4;   // default: nova
static int voice_sel_tower = 3;  // default: onyx
static int voice_sel_ground = 1; // default: echo

static const char *pattern_dir_names[] = {"left", "right"};
static int pattern_dir_selection = 0; // default: left

// ── Time ─────────────────────────────────────────────────────────
static double last_frame_time_ = 0.0;
static double get_xp_time() {
  static XPLMDataRef dr = nullptr;
  if (!dr)
    dr = XPLMFindDataRef("sim/time/total_running_time_sec");
  return dr ? static_cast<double>(XPLMGetDataf(dr)) : 0.0;
}

static size_t last_transcript_count_ = 0;
static bool window_pos_reset_pending_ = false;
static float geometry_save_timer_ = 0.0f;
static constexpr float kGeometrySaveDelay = 0.5f; // save 0.5s after last change

// ── Nearby airports panel ────────────────────────────────────────

static constexpr double kNearbyAirportsRangeNm = 40.0;
static constexpr size_t kNearbyAirportsMax = 10;

static std::vector<xplane_context::NearbyAirport> nearby_cache_;
static std::chrono::steady_clock::time_point nearby_last_refresh_{};

static void draw_nearby_airports() {
  const auto &ctx = xplane_context::get();
  const std::string &locked = xplane_context::locked_airport();

  // Throttle refresh to ~1 Hz.
  auto now = std::chrono::steady_clock::now();
  if (nearby_cache_.empty() ||
      std::chrono::duration_cast<std::chrono::milliseconds>(
          now - nearby_last_refresh_)
              .count() >= 1000) {
    nearby_cache_ = xplane_context::find_nearby_airports(kNearbyAirportsRangeNm,
                                                         kNearbyAirportsMax);
    nearby_last_refresh_ = now;
  }

  ImGui::Text("Nearby Airports (within %.0f NM)", kNearbyAirportsRangeNm);
  if (!locked.empty()) {
    ImGui::SameLine();
    if (ImGui::SmallButton("Unlock")) {
      xplane_context::unlock_airport();
      nearby_cache_.clear();
    }
  }

  // If locked airport is outside the nearby window, show it as a pinned row.
  bool locked_in_list = false;
  if (!locked.empty()) {
    for (const auto &na : nearby_cache_) {
      if (na.icao == locked) {
        locked_in_list = true;
        break;
      }
    }
  }

  auto render_row = [&](const std::string &icao, const std::string &name,
                        double dist_nm, bool has_tower, bool has_atis,
                        bool is_locked) {
    char label[256];
    std::snprintf(
        label, sizeof(label), "%s %-4s  %-24s  %5.1f NM  %s %s##nb_%s",
        is_locked ? ">" : " ", // lock marker
        icao.c_str(), name.empty() ? "" : name.substr(0, 24).c_str(), dist_nm,
        has_tower ? "T" : "-", has_atis ? "A" : "-", icao.c_str());
    if (is_locked) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 0.4f, 1.0f));
    }
    bool clicked = ImGui::Selectable(label, is_locked);
    if (is_locked)
      ImGui::PopStyleColor();

    if (clicked) {
      xplane_context::lock_airport(icao);
      // Tune standby to the most useful freq of the picked airport:
      // ATIS if available, otherwise Tower, otherwise Unicom.
      const auto &cur_ctx = xplane_context::get();
      uint32_t target_khz = 0;
      for (const auto &f : cur_ctx.airport_freqs.all) {
        if (f.type == xplane_context::FrequencyType::ATIS) {
          target_khz = f.freq_khz;
          break;
        }
      }
      if (target_khz == 0) {
        for (const auto &f : cur_ctx.airport_freqs.all) {
          if (f.type == xplane_context::FrequencyType::TOWER) {
            target_khz = f.freq_khz;
            break;
          }
        }
      }
      if (target_khz == 0) {
        for (const auto &f : cur_ctx.airport_freqs.all) {
          if (f.type == xplane_context::FrequencyType::UNICOM) {
            target_khz = f.freq_khz;
            break;
          }
        }
      }
      if (target_khz != 0 && cur_ctx.com_radio_powered)
        xplane_context::set_standby_freq(target_khz);
      nearby_cache_.clear();
    }
  };

  if (!locked.empty() && !locked_in_list) {
    // Compute distance for pinned locked row.
    double dist_nm = 0.0;
    if (ctx.airport_lat != 0.0 || ctx.airport_lon != 0.0) {
      // Reuse ctx.airport_lat/lon which was populated from the lock.
      const double kDeg2Rad = 3.14159265358979323846 / 180.0;
      double dlat = (ctx.airport_lat - ctx.latitude) * kDeg2Rad;
      double dlon = (ctx.airport_lon - ctx.longitude) * kDeg2Rad;
      double a = std::sin(dlat / 2) * std::sin(dlat / 2) +
                 std::cos(ctx.latitude * kDeg2Rad) *
                     std::cos(ctx.airport_lat * kDeg2Rad) * std::sin(dlon / 2) *
                     std::sin(dlon / 2);
      double dm =
          6371000.0 * 2.0 * std::atan2(std::sqrt(a), std::sqrt(1.0 - a));
      dist_nm = dm / 1852.0;
    }
    render_row(locked, ctx.nearest_airport_name, dist_nm,
               ctx.is_towered_airport, ctx.atis_freq_mhz > 100.0f, true);
  }

  if (nearby_cache_.empty()) {
    ImGui::TextDisabled("  (no airports in range)");
  } else {
    for (const auto &na : nearby_cache_) {
      render_row(na.icao, na.name, na.distance_nm, na.has_tower, na.has_atis,
                 na.icao == locked);
    }
  }
}

// ── Tab drawing ──────────────────────────────────────────────────

static void draw_status_tab() {
  // PTT State
  auto ptt = atc_session::ptt_state();
  std::string label = atc_session::ptt_state_label();
  if (ptt == atc_session::PTTState::RECORDING) {
    ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "%s", label.c_str());
  } else if (ptt == atc_session::PTTState::PROCESSING) {
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s", label.c_str());
  } else if (ptt == atc_session::PTTState::PLAYING) {
    ImGui::TextColored(ImVec4(0.2f, 0.8f, 1.0f, 1.0f), "%s", label.c_str());
  } else {
    ImGui::Text("%s", label.c_str());
  }

  // Flight Phase + ATC State (+ departure type when in DEPARTURE_CLEARED)
  ImGui::SameLine();
  auto cur_state = atc_state_machine::get_state();
  if (cur_state == atc_state_machine::ATCState::DEPARTURE_CLEARED) {
    ImGui::Text("   %s | %s (%s)",
                flight_phase::phase_name(flight_phase::get()),
                atc_state_machine::state_name(cur_state),
                atc_state_machine::get_departure_type_name());
  } else {
    ImGui::Text("   %s | %s", flight_phase::phase_name(flight_phase::get()),
                atc_state_machine::state_name(cur_state));
  }
  ImGui::SameLine();
  if (ImGui::SmallButton("Reset")) {
    atc_state_machine::reset();
  }

  // Last recording info
  float dur = atc_session::last_recording_duration();
  size_t samples = atc_session::last_recording_samples();
  size_t wav_bytes = atc_session::last_wav_bytes();
  if (samples > 0) {
    ImGui::Text("Last recording: %.2f s, %zu samples", dur, samples);
    ImGui::Text("WAV buffer: %zu bytes", wav_bytes);
  }

  ImGui::Separator();

  const auto &ctx = xplane_context::get();

  {
    std::string apt_display =
        ctx.nearest_airport_id.empty()
            ? "---"
            : ctx.nearest_airport_id + (ctx.nearest_airport_name.empty()
                                            ? ""
                                            : " " + ctx.nearest_airport_name);
    ImGui::Text(
        "Airport: %s %s", apt_display.c_str(),
        ctx.nearest_airport_id.empty()
            ? ""
            : (ctx.is_towered_airport ? "(Towered)" : "(Uncontrolled)"));
    if (!ctx.geometric_nearest_id.empty() &&
        ctx.geometric_nearest_id != ctx.nearest_airport_id) {
      const char *reason = xplane_context::locked_airport().empty()
                               ? "active via tuned freq"
                               : "LOCKED by picker";
      ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f),
                         "  %s | geometric nearest: %s", reason,
                         ctx.geometric_nearest_id.c_str());
    }
  }

  // Runway info
  if (!ctx.active_runway.empty()) {
    std::string rwy_list;
    for (const auto &rwy : ctx.runways) {
      if (!rwy_list.empty())
        rwy_list += ", ";
      rwy_list += rwy.end1.number + "/" + rwy.end2.number;
    }
    ImGui::Text("Runway: %s (Active)  |  Available: %s",
                ctx.active_runway.c_str(), rwy_list.c_str());
  } else if (!ctx.runways.empty()) {
    std::string rwy_list;
    for (const auto &rwy : ctx.runways) {
      if (!rwy_list.empty())
        rwy_list += ", ";
      rwy_list += rwy.end1.number + "/" + rwy.end2.number;
    }
    ImGui::Text("Runways: %s", rwy_list.c_str());
  } else {
    ImGui::TextDisabled("Runway: ---");
  }

  // Wind info
  if (ctx.wind_speed_kt < 3.0f) {
    ImGui::Text("Wind: Calm");
  } else {
    ImGui::Text("Wind: %03.0f deg @ %.0f kt", ctx.wind_direction_deg,
                ctx.wind_speed_kt);
  }

  // ATIS info
  {
    static const char *letter_names[] = {
        "Alpha",  "Bravo",    "Charlie", "Delta",  "Echo",    "Foxtrot",
        "Golf",   "Hotel",    "India",   "Juliet", "Kilo",    "Lima",
        "Mike",   "November", "Oscar",   "Papa",   "Quebec",  "Romeo",
        "Sierra", "Tango",    "Uniform", "Victor", "Whiskey", "X-ray",
        "Yankee", "Zulu"};
    char letter = atis_generator::current_letter();
    if (ctx.atis_freq_mhz > 100.0f) {
      ImGui::Text("ATIS: Information %s | %.3f MHz", letter_names[letter - 'A'],
                  ctx.atis_freq_mhz);
    } else {
      ImGui::Text("ATIS: Information %s | No freq", letter_names[letter - 'A']);
    }
  }

  float active_freq =
      (ctx.active_com == 1) ? ctx.com1_freq_mhz : ctx.com2_freq_mhz;
  ImGui::Text("COM%d: %.3f MHz (%s)", ctx.active_com, active_freq,
              xplane_context::frequency_type_name(ctx.frequency_type));
  if (ctx.tower_only) {
    ImGui::SameLine();
    ImGui::TextDisabled("(Tower-Only)");
  }

  ImGui::Text("Frequencies: %zu", ctx.airport_freqs.all.size());
  ImGui::SameLine();
  if (ImGui::SmallButton("ATC Panel")) {
    atc_panel_visible_ = !atc_panel_visible_;
  }

  ImGui::Separator();
  ImGui::Text("Position: %.4f, %.4f", ctx.latitude, ctx.longitude);
  ImGui::Text("Altitude: %.0f ft MSL", ctx.altitude_ft_msl);
  ImGui::Text("GS: %.0f kts   IAS: %.0f kts", ctx.groundspeed_kts,
              ctx.indicated_airspeed_kts);
  ImGui::Text("VS: %.0f fpm   HDG: %.0f", ctx.vertical_speed_fpm,
              ctx.heading_true);
  ImGui::Text("On Ground: %s", ctx.on_ground ? "Yes" : "No");
  ImGui::Text("Engines: %s", ctx.engines_running ? "Running" : "Off");
  ImGui::Text("Aircraft: %s",
              ctx.aircraft_icao.empty() ? "---" : ctx.aircraft_icao.c_str());

  // Last Parsed Intent
  ImGui::Separator();
  ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "Last Parsed Intent");

  const auto &pm = atc_session::last_pilot_message();
  if (!pm.raw_transcript.empty()) {
    ImGui::Text("Intent: %s", intent_parser::intent_name(pm.intent));
    ImGui::Text("Confidence: %.2f", pm.confidence);
    if (!pm.callsign.empty())
      ImGui::Text("Callsign: %s", pm.callsign.c_str());
    if (!pm.runway.empty())
      ImGui::Text("Runway: %s", pm.runway.c_str());
    ImGui::TextWrapped("Transcript: %s", pm.raw_transcript.c_str());
  } else {
    ImGui::TextDisabled("(no transcript yet)");
  }

  // Session stats
  ImGui::Separator();
  ImGui::Text("Session: %d transcriptions, %d API calls",
              atc_session::total_transcriptions(),
              atc_session::total_api_calls());

  // Warning indicators
  if (!settings::api_key_saved()) {
    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.0f, 1.0f), "[!] API key not set");
  }
}

static void draw_transcript_tab() {
  if (ImGui::Button("Clear")) {
    atc_session::clear_transcript();
    last_transcript_count_ = 0;
  }

  ImGui::Separator();

  ImGui::BeginChild("TranscriptScroll", ImVec2(0, 0), false,
                    ImGuiWindowFlags_HorizontalScrollbar);

  const auto &entries = atc_session::transcript_entries();
  for (const auto &entry : entries) {
    int mins = static_cast<int>(entry.sim_time) / 60;
    int secs = static_cast<int>(entry.sim_time) % 60;
    char line[512];
    std::string freq_tag = entry.frequency.empty() ? "" : " " + entry.frequency;
    if (entry.is_pilot) {
      std::snprintf(line, sizeof(line), "[%02d:%02d%s] You: %s", mins, secs,
                    freq_tag.c_str(), entry.text.c_str());
      ImGui::TextUnformatted(line);
    } else {
      const auto &cx = xplane_context::get();
      std::string apt = !cx.nearest_airport_name.empty()
                            ? cx.nearest_airport_name
                            : cx.nearest_airport_id;
      std::string prefix = apt.empty() ? "ATC" : apt + " ATC";
      std::snprintf(line, sizeof(line), "[%02d:%02d%s] %s: %s", mins, secs,
                    freq_tag.c_str(), prefix.c_str(), entry.text.c_str());
      ImGui::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "%s", line);
    }
  }

  // Auto-scroll on new entries
  if (entries.size() != last_transcript_count_) {
    ImGui::SetScrollHereY(1.0f);
    last_transcript_count_ = entries.size();
  }

  ImGui::EndChild();
}

// ── Audio test state ─────────────────────────────────────────────

enum class AudioTestState { IDLE, RECORDING, PLAYING };
static AudioTestState audio_test_state_ = AudioTestState::IDLE;
static float audio_test_timer_ = 0.0f;
static std::vector<uint8_t> audio_test_wav_;
static constexpr float kTestRecordDuration = 3.0f;

static void draw_audio_tab() {
  // ── Microphone / Input ──────────────────────────────────────────
  ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "Microphone");
  ImGui::TextDisabled("Input device: System Default");
  ImGui::Spacing();

  float delta = ImGui::GetIO().DeltaTime;

  if (audio_test_state_ == AudioTestState::RECORDING) {
    audio_test_timer_ += delta;
    ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f),
                       "Recording... %.1f / %.0f s", audio_test_timer_,
                       kTestRecordDuration);
    if (audio_test_timer_ >= kTestRecordDuration) {
      audio_recorder::stop_recording();
      audio_test_wav_ = audio_recorder::encode_wav();
      audio_test_timer_ = 0.0f;
      if (!audio_test_wav_.empty()) {
        char log[128];
        std::snprintf(log, sizeof(log),
                      "[xp_wellys_atc] Audio test playback — volume: %.2f, "
                      "wav: %zu bytes\n",
                      settings::volume(), audio_test_wav_.size());
        XPLMDebugString(log);

        // Save WAV to disk for debugging
        if (settings::debug_logging()) {
          std::string path = "/tmp/xp_wellys_atc_test.wav";
          FILE *f = std::fopen(path.c_str(), "wb");
          if (f) {
            std::fwrite(audio_test_wav_.data(), 1, audio_test_wav_.size(), f);
            std::fclose(f);
            char dbg[256];
            std::snprintf(dbg, sizeof(dbg),
                          "[xp_wellys_atc] Debug: test WAV saved to %s\n",
                          path.c_str());
            XPLMDebugString(dbg);
          }
        }

        audio_test_state_ = AudioTestState::PLAYING;
        audio_player::play_wav(audio_test_wav_, settings::volume());
      } else {
        audio_test_state_ = AudioTestState::IDLE;
        XPLMDebugString("[xp_wellys_atc] Audio test: WAV encode returned empty "
                        "— mic may not be working\n");
      }
    }
  } else if (audio_test_state_ == AudioTestState::PLAYING) {
    if (audio_player::is_playing()) {
      ImGui::TextColored(ImVec4(0.2f, 0.8f, 1.0f, 1.0f),
                         "Playing back test recording...");
    } else {
      audio_test_state_ = AudioTestState::IDLE;
    }
  } else {
    if (ImGui::Button("Record Test (3s)")) {
      audio_test_state_ = AudioTestState::RECORDING;
      audio_test_timer_ = 0.0f;
      audio_test_wav_.clear();
      XPLMDebugString(
          "[xp_wellys_atc] Audio test: starting 3s mic recording\n");
      audio_recorder::start_recording();
    }
    ImGui::TextDisabled(
        "Records 3 seconds and plays back to verify mic + speakers.");
  }

  ImGui::Separator();

  // ── Output / Speaker ────────────────────────────────────────────
  ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "Speaker / Output");
  ImGui::Spacing();

  // Volume
  float vol = settings::volume();
  if (ImGui::SliderFloat("Volume", &vol, 0.0f, 1.0f)) {
    settings::set_volume(vol);
    settings::save();
  }

  ImGui::TextDisabled(
      "Output: X-Plane Radio Device (Settings > Sound > Radio Device)");

  if (ImGui::Button("Test Speaker")) {
    audio_player::play_ptt_click();
  }

  ImGui::Separator();

  // ── TTS Voices ──────────────────────────────────────────────────
  ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "Text-to-Speech");
  ImGui::Spacing();
  if (ImGui::Combo("ATIS Voice", &voice_sel_atis, voice_names, voice_count)) {
    settings::set_tts_voice_atis(voice_names[voice_sel_atis]);
    settings::save();
  }
  if (ImGui::Combo("Tower Voice", &voice_sel_tower, voice_names, voice_count)) {
    settings::set_tts_voice_tower(voice_names[voice_sel_tower]);
    settings::save();
  }
  if (ImGui::Combo("Ground Voice", &voice_sel_ground, voice_names,
                   voice_count)) {
    settings::set_tts_voice_ground(voice_names[voice_sel_ground]);
    settings::save();
  }
}

static void draw_settings_tab() {
  // One-time init of buffers from settings
  if (!buffers_initialized) {
    std::strncpy(callsign_raw_buf, settings::pilot_callsign_raw().c_str(),
                 sizeof(callsign_raw_buf) - 1);
    auto init_voice_sel = [](const std::string &voice, int &sel) {
      for (int i = 0; i < voice_count; ++i) {
        if (voice == voice_names[i]) {
          sel = i;
          break;
        }
      }
    };
    init_voice_sel(settings::tts_voice_atis(), voice_sel_atis);
    init_voice_sel(settings::tts_voice_tower(), voice_sel_tower);
    init_voice_sel(settings::tts_voice_ground(), voice_sel_ground);
    std::string pdir = settings::pattern_direction();
    pattern_dir_selection = (pdir == "right") ? 1 : 0;
    buffers_initialized = true;
  }

  // API Key
  ImGui::Text("OpenAI API Key:");
  ImGui::InputText("##apikey", api_key_buf, sizeof(api_key_buf),
                   ImGuiInputTextFlags_Password);
  ImGui::SameLine();
  if (ImGui::Button("Paste")) {
    // NOLINTNEXTLINE(bugprone-command-processor)
    FILE *fp = popen("pbpaste", "r");
    if (fp) {
      char clip[256] = {};
      if (fgets(clip, sizeof(clip), fp)) {
        // Strip trailing newline
        size_t len = std::strlen(clip);
        if (len > 0 && clip[len - 1] == '\n')
          clip[len - 1] = '\0';
        std::strncpy(api_key_buf, clip, sizeof(api_key_buf) - 1);
      }
      pclose(fp);
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Save Key")) {
    if (settings::save_api_key(api_key_buf)) {
      key_just_saved = true;
      save_feedback_timer = 2.0f;
      std::memset(api_key_buf, 0, sizeof(api_key_buf));
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Delete Key")) {
    settings::delete_api_key();
    std::memset(api_key_buf, 0, sizeof(api_key_buf));
  }
  if (key_just_saved && save_feedback_timer > 0.0f) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0, 1, 0, 1), "Saved (OK)");
    save_feedback_timer -= ImGui::GetIO().DeltaTime;
    if (save_feedback_timer <= 0.0f)
      key_just_saved = false;
  } else if (settings::api_key_saved()) {
    ImGui::SameLine();
    ImGui::TextDisabled("(Key stored in Keychain)");
  }

  ImGui::Separator();

  // PTT — bound via X-Plane settings
  ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "Push-to-Talk");
  ImGui::TextDisabled("Bind via X-Plane: Settings > Joystick > Buttons & Keys");
  ImGui::TextDisabled("Command: xp_wellys_atc/ptt");
  ImGui::Separator();

  // Pilot callsign — raw registration input + phonetic preview
  if (ImGui::InputText("Callsign (Registration)", callsign_raw_buf,
                       sizeof(callsign_raw_buf))) {
    settings::set_pilot_callsign_raw(callsign_raw_buf);
  }
  std::string phonetic = settings::pilot_callsign();
  if (!phonetic.empty()) {
    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "  %s",
                       phonetic.c_str());
  } else {
    ImGui::TextDisabled("  (enter registration, e.g. HB-WRO or N342B4)");
  }

  // Pattern direction (left/right hand traffic)
  if (ImGui::Combo("Pattern Direction", &pattern_dir_selection,
                   pattern_dir_names, 2)) {
    settings::set_pattern_direction(pattern_dir_names[pattern_dir_selection]);
    settings::save();
  }

  // GPT Fallback
  bool gpt_fb = settings::gpt_fallback_enabled();
  if (ImGui::Checkbox("GPT Fallback", &gpt_fb)) {
    settings::set_gpt_fallback_enabled(gpt_fb);
  }

  // Debug logging
  bool debug = settings::debug_logging();
  if (ImGui::Checkbox("Debug Logging", &debug)) {
    settings::set_debug_logging(debug);
  }

  // Skip radio power check (workaround for exotic aircraft)
  bool skip_power = settings::skip_radio_power_check();
  if (ImGui::Checkbox("Skip Radio Power Check", &skip_power)) {
    settings::set_skip_radio_power_check(skip_power);
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "Enable if your aircraft does not report radio power correctly.\n"
        "Radio buttons and PTT will work without electrical power.");
  }

  // ATC state recovery timing
  float acf = settings::auto_correction_factor();
  char acf_label[32];
  std::snprintf(acf_label, sizeof(acf_label), "%.0f sec",
                30.0f * acf); // show base recovery time (30s * factor)
  if (ImGui::SliderFloat("ATC Recovery Time", &acf, 0.5f, 2.0f, acf_label)) {
    settings::set_auto_correction_factor(acf);
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "If you forget an ATC call (e.g. 'Runway Vacated' after landing),\n"
        "the system resets automatically after this time.\n\n"
        "Beginners: 50-60 sec (more time to think)\n"
        "Experienced: 15-20 sec (keeps the flow tight)");
  }

  // Phraseology hints toggle
  bool hints = settings::show_phraseology_hints();
  if (ImGui::Checkbox("Show Phraseology Hints", &hints)) {
    settings::set_show_phraseology_hints(hints);
  }
  if (ImGui::IsItemHovered()) {
    ImGui::SetTooltip(
        "Show suggested pilot phrases for the current ATC situation.");
  }

  // Disable Default X-Plane ATC
  bool disable_xp_atc = settings::disable_default_atc();
  if (ImGui::Checkbox("Disable Default X-Plane ATC", &disable_xp_atc)) {
    settings::set_disable_default_atc(disable_xp_atc);
  }

  ImGui::Separator();
  if (ImGui::Button("Save Settings")) {
    settings::save();
  }

  if (settings::disable_default_atc()) {
    ImGui::Separator();
    ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                       "Known conflicts with Default ATC");
    ImGui::TextWrapped(
        "Verbal ATC messages and ATC history popup are suppressed. The "
        "following X-Plane commands may still trigger default ATC behavior "
        "if bound to a key or joystick button. Unbind them in X-Plane's "
        "keyboard settings to avoid conflicts:");
    ImGui::BulletText("sim/operation/contact_atc_ptt");
    ImGui::BulletText("sim/operation/contact_atc");
    ImGui::BulletText("sim/operation/atc_readback");
    ImGui::BulletText("sim/operation/toggle_auto_checkin");
    ImGui::BulletText("sim/operation/toggle_auto_readback");
    ImGui::BulletText("sim/operation/toggle_taxi_arrows");
  }

  // About section
  ImGui::Separator();
  ImGui::Spacing();
  ImGui::TextDisabled("About");
#ifdef XP_WELLYS_ATC_VERSION
  ImGui::Text("Welly's ATC v%s", XP_WELLYS_ATC_VERSION);
#else
  ImGui::Text("Welly's ATC (dev build)");
#endif
  ImGui::Text("AI-powered ATC for X-Plane 12 VFR");
  ImGui::TextDisabled("github.com/rwellinger/xp_welly_atc");
}

// ── ATC Commands Panel ──────────────────────────────────────────

static const char *intent_display_label(const std::string &key) {
  static const std::pair<const char *, const char *> labels[] = {
      {"INITIAL_CALL_GROUND", "Initial Call (Ground)"},
      {"INITIAL_CALL_TOWER", "Initial Call (Tower)"},
      {"INITIAL_CALL_INBOUND", "Initial Call (Inbound)"},
      {"REQUEST_TAXI", "Request Taxi"},
      {"REQUEST_TAXI_PARKING", "Request Taxi to Parking"},
      {"READY_FOR_DEPARTURE", "Ready for Departure"},
      {"REPORT_POSITION", "Report Position"},
      {"REPORT_POSITION_DOWNWIND", "Report Downwind"},
      {"REPORT_POSITION_BASE", "Report Base"},
      {"REPORT_POSITION_FINAL", "Report Final"},
      {"REQUEST_LANDING", "Request Landing"},
      {"REQUEST_TOUCH_AND_GO", "Request Touch & Go"},
      {"GO_AROUND", "Go Around"},
      {"RUNWAY_VACATED", "Runway Vacated"},
      {"READBACK", "Readback"},
      {"REQUEST_FREQUENCY", "Request Frequency"},
      {"RADIO_CHECK", "Radio Check"},
      {"SELF_ANNOUNCE", "Self Announce"},
      {"INITIAL_CALL_APPROACH", "Initial Call (Approach)"},
  };
  for (const auto &p : labels)
    if (key == p.first)
      return p.second;
  return key.c_str();
}

static const char *freq_type_label(xplane_context::FrequencyType ft) {
  switch (ft) {
  case xplane_context::FrequencyType::ATIS:
    return "ATIS";
  case xplane_context::FrequencyType::DELIVERY:
    return "DEL";
  case xplane_context::FrequencyType::GROUND:
    return "GND";
  case xplane_context::FrequencyType::TOWER:
    return "TWR";
  case xplane_context::FrequencyType::APPROACH:
    return "APP";
  case xplane_context::FrequencyType::UNICOM:
    return "UNICOM";
  case xplane_context::FrequencyType::CTAF:
    return "CTAF";
  default:
    return "???";
  }
}

// ── Shared pilot action buttons (used by both tabs) ─────────────

static void draw_pilot_actions(const xplane_context::XPlaneContext &ctx,
                               bool force_towered = false) {
  if (!settings::show_phraseology_hints())
    return;

  using FT = xplane_context::FrequencyType;
  bool is_towered = force_towered || (ctx.is_towered_airport &&
                                      ctx.frequency_type != FT::UNICOM &&
                                      ctx.frequency_type != FT::CTAF);
  // When tuned to APPROACH freq (detected via airspace_db), treat as towered
  if (ctx.frequency_type == FT::APPROACH)
    is_towered = true;

  auto atc_state = atc_state_machine::get_state();
  std::string state_str = atc_state_machine::state_name(atc_state);
  auto valid = atc_templates::valid_intents(is_towered, state_str);

  // Filter by frequency type, flight phase, and post-landing context
  auto phase = flight_phase::get();
  bool post_landing =
      (atc_state == atc_state_machine::ATCState::LANDING_CLEARED ||
       atc_state == atc_state_machine::ATCState::PATTERN_ENTRY ||
       atc_state == atc_state_machine::ATCState::TOUCH_AND_GO_CLEARED) &&
      (phase == flight_phase::FlightPhase::GROUND_READY ||
       phase == flight_phase::FlightPhase::TAXI ||
       phase == flight_phase::FlightPhase::LANDING_ROLL);

  valid.erase(
      std::remove_if(
          valid.begin(), valid.end(),
          [&](const std::string &key) {
            // Frequency filter
            if (!flight_phase::is_intent_valid_for_frequency(
                    key, ctx.frequency_type)) {
              // Exception: tower-only airports allow ground intents on
              // tower freq
              if (!(ctx.tower_only && ctx.frequency_type == FT::TOWER))
                return true;
            }
            // Flight phase filter
            if (!flight_phase::check_precondition(key, phase).empty())
              return true;
            // Post-landing: hide departure hints
            if (post_landing && (key == "READY_FOR_DEPARTURE" ||
                                 key == "READY_FOR_DEPARTURE_VFR"))
              return true;
            // Departure intents only make sense after taxi
            // clearance, not in IDLE or GROUND_CONTACT
            if ((atc_state == atc_state_machine::ATCState::IDLE ||
                 atc_state == atc_state_machine::ATCState::GROUND_CONTACT) &&
                (key == "READY_FOR_DEPARTURE" ||
                 key == "READY_FOR_DEPARTURE_VFR"))
              return true;
            // On tower freq at airport with ground in IDLE:
            // pilot should contact ground first — hide all hints
            if (ctx.frequency_type == FT::TOWER &&
                atc_state == atc_state_machine::ATCState::IDLE &&
                ctx.on_ground && ctx.airport_freqs.has_ground() &&
                !ctx.tower_only)
              return true;
            return false;
          }),
      valid.end());

  // When readback is pending, only allow READBACK — ATC expects a response
  if (atc_state_machine::is_readback_pending()) {
    valid.clear();
    valid.push_back("READBACK");
  }

  // Button category for grouping
  enum class BtnCat { GROUND_OPS, TOWER_OPS, PATTERN, GENERAL };
  auto intent_category = [](const std::string &key) -> BtnCat {
    if (key == "INITIAL_CALL_GROUND" || key == "REQUEST_TAXI" ||
        key == "REQUEST_TAXI_PARKING")
      return BtnCat::GROUND_OPS;
    if (key == "INITIAL_CALL_TOWER" || key == "READY_FOR_DEPARTURE" ||
        key == "RUNWAY_VACATED")
      return BtnCat::TOWER_OPS;
    if (key == "REPORT_POSITION" || key == "REPORT_POSITION_DOWNWIND" ||
        key == "REPORT_POSITION_BASE" || key == "REPORT_POSITION_FINAL" ||
        key == "REQUEST_LANDING" || key == "REQUEST_TOUCH_AND_GO" ||
        key == "GO_AROUND" || key == "INITIAL_CALL_INBOUND" ||
        key == "INITIAL_CALL_APPROACH")
      return BtnCat::PATTERN;
    return BtnCat::GENERAL;
  };
  auto category_label = [](BtnCat cat) -> const char * {
    switch (cat) {
    case BtnCat::GROUND_OPS:
      return "Ground Operations";
    case BtnCat::TOWER_OPS:
      return "Tower Operations";
    case BtnCat::PATTERN:
      return "Pattern / Approach";
    case BtnCat::GENERAL:
      return "General";
    }
    return "";
  };

  // Sort by category
  std::stable_sort(valid.begin(), valid.end(),
                   [&](const std::string &a, const std::string &b) {
                     return static_cast<int>(intent_category(a)) <
                            static_cast<int>(intent_category(b));
                   });

  if (!valid.empty()) {
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "Phraseology Hints");
    ImGui::TextDisabled("State: %s | Phase: %s", state_str.c_str(),
                        flight_phase::phase_name(phase));

    bool radio_off = !ctx.com_radio_powered;

    // Build two var sets: short (display) and spoken (tooltip)
    intent_parser::PilotMessage dummy_msg{};
    dummy_msg.runway = ctx.active_runway;

    // Display version: short callsign (e.g. "HBAKA")
    dummy_msg.callsign = settings::pilot_callsign_raw();
    auto vars_short = atc_state_machine::build_vars(dummy_msg, ctx);

    // Spoken version: full phonetic (e.g. "Hotel Bravo Alpha Kilo Alpha")
    dummy_msg.callsign = settings::pilot_callsign();
    auto vars_spoken = atc_state_machine::build_vars(dummy_msg, ctx);

    ImGui::PushTextWrapPos(0.0f); // wrap at window edge

    BtnCat last_cat = static_cast<BtnCat>(-1);
    for (const auto &key : valid) {
      BtnCat cat = intent_category(key);
      if (cat != last_cat) {
        if (last_cat != static_cast<BtnCat>(-1))
          ImGui::Spacing();
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "%s",
                           category_label(cat));
        last_cat = cat;
      }

      // Show phraseology as text hint
      std::string phrase_tmpl = flight_phase::get_pilot_phraseology(key);
      if (!phrase_tmpl.empty()) {
        std::string display = atc_templates::fill(phrase_tmpl, vars_short);
        if (radio_off) {
          ImGui::TextDisabled("  %s", display.c_str());
        } else {
          ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.5f, 1.0f, 0.5f, 1.0f));
          ImGui::Text("  %s", display.c_str());
          ImGui::PopStyleColor();
        }
        // Tooltip: full spoken phraseology with phonetic callsign
        if (ImGui::IsItemHovered()) {
          std::string spoken = atc_templates::fill(phrase_tmpl, vars_spoken);
          ImGui::SetTooltip("Say: %s", spoken.c_str());
        }
      } else {
        const char *label = intent_display_label(key);
        ImGui::TextDisabled("  [%s]", label);
      }
    }

    ImGui::PopTextWrapPos();
  } else {
    // Context-aware empty state message
    if (atc_state == atc_state_machine::ATCState::EN_ROUTE) {
      ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "Phraseology Hints");
      ImGui::TextDisabled("Tune to an Approach frequency above");
    } else if (ctx.frequency_type == FT::TOWER &&
               atc_state == atc_state_machine::ATCState::IDLE &&
               ctx.on_ground && ctx.airport_freqs.has_ground()) {
      ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "Phraseology Hints");
      ImGui::TextDisabled("Tune to Ground frequency first");
    } else if (ctx.frequency_type == FT::ATIS ||
               ctx.frequency_type == FT::UNKNOWN) {
      ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "Phraseology Hints");
      ImGui::TextDisabled("Tune to a Ground or Tower frequency");
    }
  }
}

// ── En-Route tab state (notification hint) ──────────────────────

static size_t enroute_last_seen_count_ = 0;
static bool enroute_has_update_ = false;

// ── Airport tab content ─────────────────────────────────────────

static void draw_airport_tab(const xplane_context::XPlaneContext &ctx) {
  // Nearby airports picker
  if (ImGui::CollapsingHeader("Nearby Airports",
                              ImGuiTreeNodeFlags_DefaultOpen)) {
    draw_nearby_airports();
  }

  // VRP list (if airport has published visual reporting points)
  if (!ctx.nearest_airport_id.empty()) {
    const auto *vrp_data = airport_vrps::get(ctx.nearest_airport_id);
    if (vrp_data && !vrp_data->vrps.empty()) {
      std::string vrp_line = "VRPs: ";
      for (size_t i = 0; i < vrp_data->vrps.size(); ++i) {
        if (i > 0)
          vrp_line += " | ";
        vrp_line += vrp_data->vrps[i].name;
      }
      ImGui::TextColored(ImVec4(0.7f, 0.9f, 0.7f, 1.0f), "%s",
                         vrp_line.c_str());
    }
  }

  ImGui::Separator();

  // Frequency list with clickable buttons
  if (!ctx.airport_freqs.all.empty()) {
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "Frequencies");

    bool radio_off = !ctx.com_radio_powered;
    if (radio_off)
      ImGui::BeginDisabled();

    float active_freq =
        (ctx.active_com == 1) ? ctx.com1_freq_mhz : ctx.com2_freq_mhz;
    uint32_t active_khz =
        static_cast<uint32_t>(std::round(active_freq * 1000.0f));

    for (size_t i = 0; i < ctx.airport_freqs.all.size(); ++i) {
      const auto &af = ctx.airport_freqs.all[i];
      float freq_mhz = static_cast<float>(af.freq_khz) / 1000.0f;
      uint32_t diff = (active_khz > af.freq_khz) ? active_khz - af.freq_khz
                                                 : af.freq_khz - active_khz;
      bool is_active = (diff <= 1);

      if (is_active)
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 1.0f, 0.2f, 1.0f));

      char btn_label[96];
      const char *apt =
          ctx.nearest_airport_id.empty() ? "" : ctx.nearest_airport_id.c_str();
      std::snprintf(btn_label, sizeof(btn_label), "%s %-6s %.3f##pfreq%zu", apt,
                    freq_type_label(af.type), freq_mhz, i);

      if (ImGui::SmallButton(btn_label)) {
        xplane_context::set_standby_freq(af.freq_khz);
      }
      if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Set COM%d standby to %.3f", ctx.active_com,
                          freq_mhz);
      }

      if (is_active)
        ImGui::PopStyleColor();
    }

    if (radio_off)
      ImGui::EndDisabled();
  }

  ImGui::Separator();

  // Pilot action buttons
  draw_pilot_actions(ctx);
}

// ── En-Route tab content ────────────────────────────────────────

static void draw_enroute_tab(const xplane_context::XPlaneContext &ctx) {
  // Airspace Controllers section
  ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "Airspace Controllers");

  if (!airspace_db::ready()) {
    ImGui::TextDisabled("airspace index still loading...");
  } else if (!airspace_db::enabled()) {
    ImGui::TextDisabled(
        "airspace data missing - check XP12 Custom Data install");
  } else if (ctx.enclosing_airspaces.empty()) {
    ImGui::TextDisabled("(outside controlled airspace)");
  } else {
    float active_freq =
        (ctx.active_com == 1) ? ctx.com1_freq_mhz : ctx.com2_freq_mhz;
    uint32_t active_khz =
        static_cast<uint32_t>(std::round(active_freq * 1000.0f));

    bool radio_off = !ctx.com_radio_powered;
    if (radio_off)
      ImGui::BeginDisabled();

    for (size_t ci = 0; ci < ctx.enclosing_airspaces.size(); ++ci) {
      const auto *c = ctx.enclosing_airspaces[ci];

      // Controller header line
      const char *role_label = airspace_db::role_name(c->role);
      ImGui::Text("%s %s [%s] %d-%d ft", role_label, c->name.c_str(),
                  c->facility_id.c_str(), c->floor_ft, c->ceiling_ft);

      // Clickable frequency buttons
      const char *type_short =
          (c->role == airspace_db::ControllerRole::TRACON) ? "APP" : "INFO";
      for (size_t fi = 0; fi < c->freqs_khz.size() && fi < 4; ++fi) {
        uint32_t freq = c->freqs_khz[fi];
        float freq_mhz = static_cast<float>(freq) / 1000.0f;
        uint32_t diff =
            (active_khz > freq) ? active_khz - freq : freq - active_khz;
        bool is_active = (diff <= 1);

        if (is_active)
          ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.2f, 1.0f, 0.2f, 1.0f));

        char btn_label[96];
        std::snprintf(btn_label, sizeof(btn_label),
                      "%s %-4s %.3f##afreq%zu_%zu", c->facility_id.c_str(),
                      type_short, freq_mhz, ci, fi);

        if (ImGui::SmallButton(btn_label)) {
          xplane_context::set_standby_freq(freq);
        }
        if (ImGui::IsItemHovered()) {
          ImGui::SetTooltip("Set COM%d standby to %.3f", ctx.active_com,
                            freq_mhz);
        }

        if (is_active)
          ImGui::PopStyleColor();
      }
      if (c->freqs_khz.size() > 4)
        ImGui::TextDisabled("  ... +%zu more", c->freqs_khz.size() - 4);

      ImGui::Spacing();
    }

    if (radio_off)
      ImGui::EndDisabled();
  }

  // Guidance banner
  auto atc_state = atc_state_machine::get_state();
  if (atc_state == atc_state_machine::ATCState::EN_ROUTE &&
      !ctx.enclosing_airspaces.empty()) {
    for (const auto *c : ctx.enclosing_airspaces) {
      if (c->role == airspace_db::ControllerRole::TRACON) {
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.4f, 0.9f, 1.0f, 1.0f),
                           "-> Contact %s Approach", c->name.c_str());
        ImGui::TextDisabled("   tune to a frequency above");
        break;
      }
    }
  }

  ImGui::Separator();

  // Pilot action buttons (with force_towered when on approach freq)
  draw_pilot_actions(ctx);
}

// ── ATC Commands Panel (tabbed) ─────────────────────────────────

static void draw_atc_panel() {
  if (!atc_panel_visible_)
    return;

  ImGui::SetNextWindowSize(ImVec2(420, 620), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(280, 300), ImVec2(700, 1000));

  bool open = atc_panel_visible_;
  if (ImGui::Begin("ATC Commands##atc_panel", &open,
                   ImGuiWindowFlags_NoCollapse)) {
    const auto &ctx = xplane_context::get();

    // Airport header (always visible above tabs)
    {
      std::string apt_label =
          ctx.nearest_airport_id.empty()
              ? "---"
              : ctx.nearest_airport_id + (ctx.nearest_airport_name.empty()
                                              ? ""
                                              : " " + ctx.nearest_airport_name);
      const char *type_label =
          ctx.is_towered_airport
              ? (ctx.tower_only ? "(Tower-Only)" : "(Towered)")
              : "(Uncontrolled)";
      bool tuned_elsewhere = !ctx.geometric_nearest_id.empty() &&
                             ctx.geometric_nearest_id != ctx.nearest_airport_id;
      if (tuned_elsewhere) {
        ImGui::Text("%s %s", apt_label.c_str(), type_label);
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f),
                           "  tuned (geom nearest: %s)",
                           ctx.geometric_nearest_id.c_str());
      } else {
        ImGui::Text("%s %s", apt_label.c_str(), type_label);
      }
    }

    // Radio power warning (only when unpowered)
    if (!ctx.com_radio_powered) {
      ImGui::Spacing();
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
      ImGui::TextWrapped(
          "COM%d radio has no power - ATC disabled. Turn on "
          "avionics/battery or enable 'Skip Radio Power Check' in Settings.",
          ctx.active_com);
      ImGui::PopStyleColor();
      ImGui::Spacing();
    }

    // ATIS summary (always visible above tabs)
    {
      static const char *letter_names[] = {
          "Alpha",  "Bravo",    "Charlie", "Delta",  "Echo",    "Foxtrot",
          "Golf",   "Hotel",    "India",   "Juliet", "Kilo",    "Lima",
          "Mike",   "November", "Oscar",   "Papa",   "Quebec",  "Romeo",
          "Sierra", "Tango",    "Uniform", "Victor", "Whiskey", "X-ray",
          "Yankee", "Zulu"};
      char letter = atis_generator::current_letter();
      int qnh_hpa = static_cast<int>(std::round(ctx.qnh_inhg * 33.8639f));
      ImGui::Text("ATIS: %s | RWY %s | QNH %d", letter_names[letter - 'A'],
                  ctx.active_runway.empty() ? "---" : ctx.active_runway.c_str(),
                  qnh_hpa);
      if (ctx.wind_speed_kt < 3.0f) {
        ImGui::Text("Wind: calm");
      } else {
        ImGui::Text("Wind: %03.0f deg / %.0f kt", ctx.wind_direction_deg,
                    ctx.wind_speed_kt);
      }
    }

    ImGui::Separator();

    // Track airspace changes for En-Route tab notification
    size_t current_airspace_count = ctx.enclosing_airspaces.size();
    if (current_airspace_count != enroute_last_seen_count_) {
      enroute_has_update_ = true;
    }

    // Tab bar
    if (ImGui::BeginTabBar("ATC_Tabs")) {
      // Airport tab
      if (ImGui::BeginTabItem("Airport")) {
        draw_airport_tab(ctx);
        ImGui::EndTabItem();
      }

      // En-Route tab (with notification hint)
      bool hint_colored = false;
      if (enroute_has_update_) {
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.9f, 1.0f, 1.0f));
        hint_colored = true;
      }
      if (ImGui::BeginTabItem("En-Route")) {
        // Mark notification as seen
        enroute_has_update_ = false;
        enroute_last_seen_count_ = current_airspace_count;
        if (hint_colored) {
          ImGui::PopStyleColor();
          hint_colored = false;
        }
        draw_enroute_tab(ctx);
        ImGui::EndTabItem();
      }
      if (hint_colored)
        ImGui::PopStyleColor();

      ImGui::EndTabBar();
    }

    ImGui::Separator();

    // Last ATC response (always visible below tabs)
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "Last ATC");
    std::string last_atc = atc_session::last_atc_response();
    if (!last_atc.empty()) {
      ImGui::TextWrapped("%s", last_atc.c_str());
    } else {
      ImGui::TextDisabled("(no ATC response yet)");
    }
  }
  ImGui::End();

  if (!open)
    atc_panel_visible_ = false;
}

// ── XPLM window callbacks (input capture only) ──────────────────

static void wnd_draw_cb(XPLMWindowID, void *) {
  // Nothing — rendering happens in the draw phase callback
}

// Pass-through helper: return 1 only when ImGui actually wants the mouse
// (cursor over an ImGui window). Otherwise return 0 so X-Plane processes the
// click for cockpit manipulators. WantCaptureMouse reflects the previous
// frame's hit-test, which is sufficient because draw_phase_cb feeds the mouse
// position every frame.
static bool imgui_wants_mouse_at(XPLMWindowID wnd, int x, int y) {
  int left, top, right, bottom;
  XPLMGetWindowGeometry(wnd, &left, &top, &right, &bottom);
  ImGuiIO &io = ImGui::GetIO();
  io.AddMousePosEvent(static_cast<float>(x - left),
                      static_cast<float>(top - y));
  return io.WantCaptureMouse;
}

static int wnd_mouse_cb(XPLMWindowID wnd, int x, int y, XPLMMouseStatus status,
                        void *) {
  if (!imgui_wants_mouse_at(wnd, x, y))
    return 0; // pass through to X-Plane (cockpit manipulation)
  ImGuiIO &io = ImGui::GetIO();
  if (status == xplm_MouseDown)
    io.AddMouseButtonEvent(0, true);
  if (status == xplm_MouseUp)
    io.AddMouseButtonEvent(0, false);
  return 1;
}

static int wnd_rclick_cb(XPLMWindowID wnd, int x, int y, XPLMMouseStatus status,
                         void *) {
  if (!imgui_wants_mouse_at(wnd, x, y))
    return 0;
  ImGuiIO &io = ImGui::GetIO();
  if (status == xplm_MouseDown)
    io.AddMouseButtonEvent(1, true);
  if (status == xplm_MouseUp)
    io.AddMouseButtonEvent(1, false);
  return 1;
}

static int wnd_wheel_cb(XPLMWindowID wnd, int x, int y, int, int clicks,
                        void *) {
  if (!imgui_wants_mouse_at(wnd, x, y))
    return 0;
  ImGui::GetIO().AddMouseWheelEvent(0.0f, static_cast<float>(clicks));
  return 1;
}

static XPLMCursorStatus wnd_cursor_cb(XPLMWindowID, int, int, void *) {
  return xplm_CursorDefault;
}

static void wnd_key_cb(XPLMWindowID, char key, XPLMKeyFlags flags, char vkey,
                       void *, int losing_focus) {
  if (losing_focus) {
    ImGui::GetIO().AddFocusEvent(false);
    return;
  }
  ImGuiIO &io = ImGui::GetIO();
  // Only consume keys when ImGui has an active text input.
  // Otherwise let X-Plane handle them (command key bindings, etc.)
  if (!io.WantTextInput)
    return;
  bool is_down = (flags & xplm_DownFlag) != 0;
  bool is_up = (flags & xplm_UpFlag) != 0;
  // Map special keys for both press and release so ImGui doesn't get stuck
  // with a "held" key (which would cause e.g. Backspace to keep deleting).
  ImGuiKey ikey = ImGuiKey_None;
  if (vkey == XPLM_VK_BACK)
    ikey = ImGuiKey_Backspace;
  else if (vkey == XPLM_VK_DELETE)
    ikey = ImGuiKey_Delete;
  else if (vkey == XPLM_VK_RETURN)
    ikey = ImGuiKey_Enter;
  else if (vkey == XPLM_VK_LEFT)
    ikey = ImGuiKey_LeftArrow;
  else if (vkey == XPLM_VK_RIGHT)
    ikey = ImGuiKey_RightArrow;
  else if (vkey == XPLM_VK_HOME)
    ikey = ImGuiKey_Home;
  else if (vkey == XPLM_VK_END)
    ikey = ImGuiKey_End;
  else if (vkey == XPLM_VK_TAB)
    ikey = ImGuiKey_Tab;
  if (ikey != ImGuiKey_None) {
    if (is_down)
      io.AddKeyEvent(ikey, true);
    if (is_up)
      io.AddKeyEvent(ikey, false);
  }
  if (is_down && key >= 32 && key < 127)
    io.AddInputCharacter(static_cast<unsigned>(key));
  if (is_down && vkey == XPLM_VK_ESCAPE) {
    visible = false;
    if (window_id) {
      XPLMSetWindowIsVisible(window_id, 0);
      XPLMTakeKeyboardFocus(nullptr);
    }
  }
}

// ── Draw phase callback (ImGui rendering) ────────────────────────

static int draw_phase_cb(XPLMDrawingPhase, int, void *) {
  if (!visible && !atc_panel_visible_)
    return 1;

  int gl, gt, gr, gb;
  XPLMGetScreenBoundsGlobal(&gl, &gt, &gr, &gb);
  int sw = gr - gl;
  int sh = gt - gb;
  if (sw <= 0 || sh <= 0)
    return 1;

  // Keep capture window sized to full screen
  if (window_id) {
    int wl, wt, wr, wb;
    XPLMGetWindowGeometry(window_id, &wl, &wt, &wr, &wb);
    if (wl != gl || wb != gb || wr != gr || wt != gt)
      XPLMSetWindowGeometry(window_id, gl, gt, gr, gb);
  }

  // Save GL state
  GLint prev_viewport[4];
  glGetIntegerv(GL_VIEWPORT, prev_viewport);
  glPushAttrib(GL_TRANSFORM_BIT | GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT |
               GL_DEPTH_BUFFER_BIT | GL_SCISSOR_BIT | GL_TEXTURE_BIT);
  glMatrixMode(GL_PROJECTION);
  glPushMatrix();
  glMatrixMode(GL_MODELVIEW);
  glPushMatrix();

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_LIGHTING);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  glViewport(0, 0, sw, sh);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glOrtho(0, sw, sh, 0, -1, 1); // top-left origin for ImGui
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();

  // ImGui frame setup
  ImGuiIO &io = ImGui::GetIO();
  double now = get_xp_time();
  io.DeltaTime = static_cast<float>(std::max(now - last_frame_time_, 0.001));
  last_frame_time_ = now;
  io.DisplaySize = ImVec2(static_cast<float>(sw), static_cast<float>(sh));

  // Track mouse position every frame (hover support)
  int gmx, gmy;
  XPLMGetMouseLocationGlobal(&gmx, &gmy);
  io.AddMousePosEvent(static_cast<float>(gmx - gl),
                      static_cast<float>(gt - gmy));

  ImGui_ImplOpenGL2_NewFrame();
  ImGui::NewFrame();

  // Re-claim keyboard focus whenever ImGui wants text input (prevents X-Plane
  // from stealing focus while the user is typing in an input field)
  if (ImGui::GetIO().WantTextInput && window_id) {
    XPLMTakeKeyboardFocus(window_id);
  }

  // Window position/size — load from settings or center
  if (window_pos_reset_pending_) {
    // Force re-center on next frame
    float def_w = 500.0f, def_h = 450.0f;
    ImGui::SetNextWindowPos(ImVec2((static_cast<float>(sw) - def_w) * 0.5f,
                                   (static_cast<float>(sh) - def_h) * 0.5f),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(def_w, def_h), ImGuiCond_Always);
    window_pos_reset_pending_ = false;
  } else {
    float sx = settings::window_x();
    float sy = settings::window_y();
    float sw_s = settings::window_w();
    float sh_s = settings::window_h();
    if (sx >= 0.0f && sy >= 0.0f) {
      ImGui::SetNextWindowPos(ImVec2(sx, sy), ImGuiCond_FirstUseEver);
    } else {
      float def_w = 500.0f, def_h = 450.0f;
      ImGui::SetNextWindowPos(ImVec2((static_cast<float>(sw) - def_w) * 0.5f,
                                     (static_cast<float>(sh) - def_h) * 0.5f),
                              ImGuiCond_FirstUseEver);
    }
    if (sw_s > 0.0f && sh_s > 0.0f) {
      ImGui::SetNextWindowSize(ImVec2(sw_s, sh_s), ImGuiCond_FirstUseEver);
    } else {
      ImGui::SetNextWindowSize(ImVec2(500.0f, 450.0f), ImGuiCond_FirstUseEver);
    }
  }
  ImGui::SetNextWindowSizeConstraints(ImVec2(400, 300), ImVec2(1920, 1080));

  // Main window (only when visible)
  bool open = visible;
  if (visible) {
#ifdef XP_WELLYS_ATC_VERSION
    static const std::string window_title =
        std::string("Welly's ATC v") + XP_WELLYS_ATC_VERSION + "##main";
#else
    static const std::string window_title = "Welly's ATC##main";
#endif
    if (ImGui::Begin(window_title.c_str(), &open,
                     ImGuiWindowFlags_NoCollapse)) {
      if (ImGui::BeginTabBar("MainTabs")) {
        if (ImGui::BeginTabItem("Status")) {
          draw_status_tab();
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Transcript")) {
          draw_transcript_tab();
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Settings")) {
          draw_settings_tab();
          ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Audio")) {
          draw_audio_tab();
          ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
      }

      // Save window geometry when moved/resized (debounced)
      ImVec2 pos = ImGui::GetWindowPos();
      ImVec2 size = ImGui::GetWindowSize();
      float prev_x = settings::window_x();
      float prev_y = settings::window_y();
      float prev_w = settings::window_w();
      float prev_h = settings::window_h();
      if (pos.x != prev_x || pos.y != prev_y || size.x != prev_w ||
          size.y != prev_h) {
        settings::set_window_geometry(pos.x, pos.y, size.x, size.y);
        geometry_save_timer_ = kGeometrySaveDelay;
      }
      if (geometry_save_timer_ > 0.0f) {
        geometry_save_timer_ -= ImGui::GetIO().DeltaTime;
        if (geometry_save_timer_ <= 0.0f) {
          settings::save();
          geometry_save_timer_ = 0.0f;
        }
      }
    }
    ImGui::End();

    if (!open) {
      visible = false;
      if (window_id) {
        XPLMSetWindowIsVisible(window_id, 0);
        XPLMTakeKeyboardFocus(nullptr);
      }
    }
  } // end if (visible)

  // ATC Commands panel (independent of main window)
  draw_atc_panel();

  // If both windows are now closed, release the capture window so X-Plane
  // gets input back. Without this, the invisible full-screen capture window
  // swallows all mouse/keyboard events.
  if (!visible && !atc_panel_visible_ && window_id) {
    XPLMSetWindowIsVisible(window_id, 0);
    XPLMTakeKeyboardFocus(nullptr);
  }

  ImGui::Render();
  ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());

  // Restore GL state
  glMatrixMode(GL_PROJECTION);
  glPopMatrix();
  glMatrixMode(GL_MODELVIEW);
  glPopMatrix();
  glPopAttrib();
  glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2],
             prev_viewport[3]);

  return 1;
}

// ── Public lifecycle ─────────────────────────────────────────────

void init() {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();

  ImGuiIO &io = ImGui::GetIO();
  static std::string ini_path = settings::get_data_dir() + "/imgui.ini";
  io.IniFilename = ini_path.c_str();
  io.LogFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

  ImGui::StyleColorsDark();
  auto &style = ImGui::GetStyle();
  style.WindowRounding = 6.0f;
  style.FrameRounding = 3.0f;
  style.WindowPadding = ImVec2(8, 6);

  ImGui_ImplOpenGL2_Init();
  last_frame_time_ = get_xp_time();

  XPLMRegisterDrawCallback(draw_phase_cb, xplm_Phase_Window, 1, nullptr);
}

void stop() {
  XPLMUnregisterDrawCallback(draw_phase_cb, xplm_Phase_Window, 1, nullptr);

  if (window_id) {
    XPLMDestroyWindow(window_id);
    window_id = nullptr;
  }
  ImGui_ImplOpenGL2_Shutdown();
  ImGui::DestroyContext();

  buffers_initialized = false;
}

void toggle() {
  visible = !visible;

  if (visible && !window_id) {
    // Create full-screen invisible capture window
    int gl, gt, gr, gb;
    XPLMGetScreenBoundsGlobal(&gl, &gt, &gr, &gb);

    XPLMCreateWindow_t p{};
    p.structSize = sizeof(p);
    p.left = gl;
    p.bottom = gb;
    p.right = gr;
    p.top = gt;
    p.visible = 1;
    p.drawWindowFunc = wnd_draw_cb;
    p.handleMouseClickFunc = wnd_mouse_cb;
    p.handleKeyFunc = wnd_key_cb;
    p.handleCursorFunc = wnd_cursor_cb;
    p.handleMouseWheelFunc = wnd_wheel_cb;
    p.handleRightClickFunc = wnd_rclick_cb;
    p.refcon = nullptr;
    p.decorateAsFloatingWindow = xplm_WindowDecorationNone;
    p.layer = xplm_WindowLayerFloatingWindows;
    window_id = XPLMCreateWindowEx(&p);

    if (settings::debug_logging()) {
      char dbg[256];
      std::snprintf(dbg, sizeof(dbg),
                    "[xp_wellys_atc] Capture window created: "
                    "bounds(%d,%d,%d,%d) wnd=%p\n",
                    gl, gt, gr, gb, static_cast<void *>(window_id));
      XPLMDebugString(dbg);
    }
  }

  if (window_id) {
    XPLMSetWindowIsVisible(window_id, visible ? 1 : 0);
    if (visible) {
      XPLMBringWindowToFront(window_id);
      XPLMTakeKeyboardFocus(window_id);
    } else {
      XPLMTakeKeyboardFocus(nullptr); // release focus
    }
  }
}

void draw() {
  // Rendering now handled by draw_phase_cb
}

void toggle_atc_panel() {
  atc_panel_visible_ = !atc_panel_visible_;

  // Ensure capture window exists for input events
  if (atc_panel_visible_ && !window_id && !visible) {
    // Will be created on next toggle() or when main window opens
    // For now, the panel renders but may not capture input without the
    // capture window. Open the capture window if needed.
    int gl, gt, gr, gb;
    XPLMGetScreenBoundsGlobal(&gl, &gt, &gr, &gb);

    XPLMCreateWindow_t p{};
    p.structSize = sizeof(p);
    p.left = gl;
    p.bottom = gb;
    p.right = gr;
    p.top = gt;
    p.visible = 1;
    p.drawWindowFunc = wnd_draw_cb;
    p.handleMouseClickFunc = wnd_mouse_cb;
    p.handleKeyFunc = wnd_key_cb;
    p.handleCursorFunc = wnd_cursor_cb;
    p.handleMouseWheelFunc = wnd_wheel_cb;
    p.handleRightClickFunc = wnd_rclick_cb;
    p.refcon = nullptr;
    p.decorateAsFloatingWindow = xplm_WindowDecorationNone;
    p.layer = xplm_WindowLayerFloatingWindows;
    window_id = XPLMCreateWindowEx(&p);
  }

  if (window_id) {
    if (atc_panel_visible_ || visible) {
      XPLMSetWindowIsVisible(window_id, 1);
      XPLMBringWindowToFront(window_id);
      // Only take keyboard focus if main window needs text input.
      // ATC panel alone has no text fields — taking focus would block
      // X-Plane command key bindings (including the toggle key itself).
      if (visible)
        XPLMTakeKeyboardFocus(window_id);
    } else {
      XPLMSetWindowIsVisible(window_id, 0);
      XPLMTakeKeyboardFocus(nullptr);
    }
  }
}

void reset_window_position() {
  settings::reset_window_geometry();
  window_pos_reset_pending_ = true;
  // Open the window so the user can see it
  if (!visible)
    toggle();
}

} // namespace atc_ui
