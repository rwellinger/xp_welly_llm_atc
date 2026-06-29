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

#include "atc/atc_session.hpp"
#include "atc/atc_state_machine.hpp"
#include "atc/atis_generator.hpp"
#include "atc/engine.hpp"
#include "atc/flight_phase.hpp"
#include "atc/atc_templates.hpp"
#include "atc/intent_parser.hpp"
#include "audio/audio_player.hpp"
#include "audio/audio_recorder.hpp"
#include "backends/manager.hpp"
#include "core/logging.hpp"
#include "core/xplane_context.hpp"
#include "data/airspace_db.hpp"
#include "data/simbrief_ofp.hpp"
#include "persistence/model_manifest.hpp"
#include "persistence/model_paths.hpp"
#include "persistence/settings.hpp"

#include <XPLMProcessing.h>
#include <XPLMUtilities.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <functional>
#include <sstream>
#include <string>
#include <utility>

namespace atc_session {

static PTTState state_ = PTTState::IDLE;

// True between speak_response() submitting a TTS job and the async
// callback firing. Without this flag, update() flips PLAYING->IDLE in
// the window where TTS is still synthesising (audio_player::is_playing()
// returns false because play_pcm hasn't been called yet) — which let
// the per-tick traffic advisor poll fire and stack a fresh advisory on
// top of the ack TTS that hadn't been spoken yet. Drained on the main
// thread by the manager's callback queue, so no atomic is needed.
static bool tts_pending_ = false;

static float last_duration_ = 0.0f;
static size_t last_samples_ = 0;
static size_t last_wav_bytes_ = 0;

static std::vector<TranscriptEntry> transcript_;
static intent_parser::PilotMessage last_pilot_message_;

// Transcript log file — opened (overwritten) at each session init.
// Path: <plugin>/Resources/transcript.log
static FILE *g_transcript_log_ = nullptr;
static std::string g_last_stt_model_;

// Returns the human-readable name of the active STT model.
static std::string current_stt_model_label() {
  const std::string &mode = settings::backend_mode();
  if (mode == "local" || mode == "local_stt_mistral")
    return settings::local_stt_model();
  if (mode == "mistral")
    return settings::mistral_stt_model();
  if (mode == "openai")
    return settings::openai_stt_model();
  return mode;
}

static void write_stt_header_if_changed() {
  if (!g_transcript_log_)
    return;
  const std::string label = current_stt_model_label();
  if (label == g_last_stt_model_)
    return;
  std::fprintf(g_transcript_log_, "-- STT: %s --\n", label.c_str());
  std::fflush(g_transcript_log_);
  g_last_stt_model_ = label;
}

static void push_transcript(TranscriptEntry e) {
  const auto &ctx = xplane_context::get();
  e.lat = ctx.latitude;
  e.lon = ctx.longitude;
  e.alt_ft = ctx.altitude_ft_msl;
  e.heading = ctx.heading_true;

  if (g_transcript_log_) {
    int mins = static_cast<int>(e.sim_time) / 60;
    int secs = static_cast<int>(e.sim_time) % 60;
    const char *freq = e.frequency.empty() ? "" : e.frequency.c_str();
    // Position suffix appended to every line for post-flight debugging.
    int qnh_hpa = ctx.qnh_hpa;
    char pos[120];
    std::snprintf(pos, sizeof(pos),
                  " @(%.4f,%.4f alt=%.0fft pa=%.0fft hdg=%.0f qnh=%d sqk=%04d mode=%d)",
                  e.lat, e.lon, e.alt_ft, ctx.pressure_alt_ft, e.heading, qnh_hpa,
                  ctx.transponder_code, ctx.transponder_mode);
    switch (e.kind) {
    case TranscriptKind::Pilot:
      write_stt_header_if_changed();
      std::fprintf(g_transcript_log_, "[%02d:%02d%s%s] You: %s%s\n", mins, secs,
                   e.frequency.empty() ? "" : " ", freq, e.text.c_str(), pos);
      break;
    case TranscriptKind::Tower:
      std::fprintf(g_transcript_log_, "[%02d:%02d%s%s] %s: %s%s\n", mins, secs,
                   e.frequency.empty() ? "" : " ", freq,
                   e.label.empty() ? "ATC" : e.label.c_str(), e.text.c_str(),
                   pos);
      break;
    case TranscriptKind::System:
      std::fprintf(g_transcript_log_, "[%02d:%02d] -- %s --%s\n", mins, secs,
                   e.text.c_str(), pos);
      break;
    }
    std::fflush(g_transcript_log_);
  }
  transcript_.push_back(std::move(e));
}

// Capture the controller label that should appear in the transcript for the
// current ATC message — stored per-entry so that historical messages are not
// retroactively relabelled when the active controller changes.
static std::string current_tower_label() {
  const std::string &ctrl = engine::current_controller_label();
  if (!ctrl.empty())
    return ctrl;
  // For IFR airborne states: use the pending departure label stored when the
  // takeoff clearance was issued. This covers the window between takeoff and
  // when poll_departure_handoff() fires and activates the label officially.
  using AS = atc_state_machine::ATCState;
  const auto st = atc_state_machine::get_state();
  if (st == AS::IFR_FREQ_HANDOFF || st == AS::IFR_EN_ROUTE ||
      st == AS::IFR_RADAR_CONTACT || st == AS::IFR_ENROUTE_CRUISE) {
    const std::string &pending = engine::pending_departure_label();
    if (!pending.empty())
      return pending;
  }
  // En-route IFR states: nearest airport is irrelevant to the sector —
  // return "Control" until the real centre label is populated by polling.
  if (st == AS::IFR_ENROUTE_CRUISE || st == AS::IFR_EN_ROUTE ||
      st == AS::IFR_FREQ_HANDOFF || st == AS::IFR_RADAR_CONTACT ||
      st == AS::IFR_APPROACH_CONTACT || st == AS::IFR_APPROACH_DESCENT ||
      st == AS::IFR_APPROACH_TOWER)
    return "Control";
  const auto &cx = xplane_context::get();
  const std::string &name = cx.nearest_airport_name;
  const std::string &id = cx.nearest_airport_id;
  std::string apt = !name.empty() ? name : id;
  // City name only — strip local suffix ("Annecy Meythet" → "Annecy")
  auto sep = apt.find_first_of(" -");
  if (sep != std::string::npos)
    apt = apt.substr(0, sep);
  return apt.empty() ? "ATC" : apt + " ATC";
}
static int total_transcriptions_ = 0;
static int total_inferences_ = 0;
static constexpr float kMinRecordingDuration = 0.5f;
// Extra mic-open time after PTT release to avoid cutting the last syllable.
static constexpr float kPttTailSec = 0.60f;
static float ptt_tail_remaining_ = 0.0f;

// ATIS playback state
static bool atis_playing_ = false;
// COM (1 or 2) the active ATIS broadcast started on. Pinned at trigger
// time so the abort check stays consistent if the pilot rapidly cycles
// COM2 while COM1 is also on ATIS — we abort the stream that's playing,
// not whichever COM still happens to be on the ATIS freq.
static int atis_active_com_ = 0;
static float atis_cooldown_ = 0.0f;
// 120 s cooldown: an inbound VFR pilot at LSZB tunes ATIS, then APP, then
// TWR within ~30-90 s. With a 30 s cooldown the previous-airport's ATIS
// re-fires when the pilot retunes ATIS during that sequence — annoying
// and unrealistic. 120 s is long enough to span a normal arrival
// sequence but short enough that a pilot who deliberately retunes ATIS
// minutes later (e.g. checking for a new letter) still gets fresh
// playback.
static constexpr float kAtisCooldownSec = 120.0f;
static float atis_tuned_timer_ = 0.0f;           // how long tuned to ATIS freq
static constexpr float kAtisTuneDelaySec = 2.0f; // wait before playing

// Map the pilot's currently-tuned frequency to a logical voice role.
// The transmitting controller is whichever one owns the freq the pilot
// is listening to — *not* the state machine's next_state. Without this,
// a Ground handoff message ("contact Tower on 120.100") gets spoken
// with the Tower voice, because the state has already advanced to
// TOWER_CONTACT by the time speak_response runs.
//
// Tower-only airports collapse Ground/Approach onto the Tower voice
// (one controller handles everything on the tower freq). Unknown /
// Center / unicom-class freqs fall back to the Center voice — that's
// the en-route facility a pilot would talk to between airports.
static model_manifest::VoiceRole
role_for_frequency(const xplane_context::XPlaneContext &ctx) {
  using FT = xplane_context::FrequencyType;
  using R = model_manifest::VoiceRole;
  if (ctx.tower_only)
    return R::Tower;
  switch (ctx.frequency_type) {
  case FT::ATIS:
    return R::Atis;
  case FT::DELIVERY:
  case FT::GROUND:
    return R::Ground;
  case FT::TOWER:
    return R::Tower;
  case FT::APPROACH:
  case FT::DEPARTURE:
  case FT::UNICOM:
  case FT::CTAF:
  case FT::UNKNOWN:
    return R::Center;
  }
  return R::Center;
}

// 5-letter all-uppercase tokens are ICAO waypoint fixes (e.g. "BULOL", "ROMAM").
// espeak-ng spells out all-caps sequences letter-by-letter; lowercasing forces
// phonetic pronunciation, which is how controllers pronounce 5-letter fixes.
// 2-4 letter uppercase codes (VOR/NDB, ILS, etc.) are left as-is — those are
// correctly spelled out letter-by-letter in real ATC phraseology.
static std::string expand_navfix_names(std::string s) {
  size_t i = 0;
  while (i < s.size()) {
    if (!std::isupper(static_cast<unsigned char>(s[i]))) { ++i; continue; }
    size_t j = i;
    while (j < s.size() && std::isupper(static_cast<unsigned char>(s[j]))) ++j;
    bool left_ok  = (i == 0) || !std::isalnum(static_cast<unsigned char>(s[i - 1]));
    bool right_ok = (j >= s.size()) || !std::isalnum(static_cast<unsigned char>(s[j]));
    if (left_ok && right_ok && (j - i) == 5) {
      for (size_t k = i; k < j; ++k)
        s[k] = static_cast<char>(std::tolower(static_cast<unsigned char>(s[k])));
    }
    i = (j > i) ? j : i + 1;
  }
  return s;
}

// Runway designator expansion: "runway 04L" -> "runway 04 Left", "22R" -> "22 Right",
// "12C" -> "12 Center". Only expands after "runway " so waypoint idents
// (e.g. ABDI8R, FN04A) are never affected.
static std::string expand_runways(std::string s) {
  const std::string kw = "runway ";
  size_t pos = 0;
  while ((pos = s.find(kw, pos)) != std::string::npos) {
    size_t j = pos + kw.size();
    while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j])))
      ++j;
    if (j > pos + kw.size() && j < s.size()) {
      char c = static_cast<char>(
          std::toupper(static_cast<unsigned char>(s[j])));
      bool word_end = (j + 1 >= s.size()) ||
                      !std::isalnum(static_cast<unsigned char>(s[j + 1]));
      if (word_end) {
        if (c == 'L') { s.replace(j, 1, " Left");   pos = j + 5; continue; }
        if (c == 'R') { s.replace(j, 1, " Right");  pos = j + 6; continue; }
        if (c == 'C') { s.replace(j, 1, " Center"); pos = j + 7; continue; }
      }
    }
    pos += kw.size();
  }
  return s;
}

// Speak ATC response via local TTS, then transition to PLAYING → IDLE.
// `length_scale` > 1.0 makes Piper speak slower (used for ATIS).
// `on_playback_starting` (optional) fires on the main thread the moment
// audio is about to play — only after a successful synthesis. Used by
// the ATIS path to delay the transcript line until the user actually
// hears the broadcast, so a silent TTS failure does not leave a ghost
// entry in the history.
static void
speak_response(const std::string &text, model_manifest::VoiceRole role,
               float length_scale = 1.0f, int com_override = 0,
               std::function<void()> on_playback_starting = nullptr) {
  state_ = PTTState::PLAYING;
  tts_pending_ = true;
  ++total_inferences_; // TTS inference

  std::string final_text = expand_navfix_names(expand_runways(text));

  backends::tts::synthesize_async(
      final_text, role, length_scale,
      [com_override, on_playback_starting = std::move(on_playback_starting)](
          backends::tts::Audio audio, bool success) {
        tts_pending_ = false;
        if (success && !audio.pcm16.empty()) {
          if (settings::debug_logging()) {
            char dbg[160];
            std::snprintf(dbg, sizeof(dbg),
                          "[xp_wellys_atc][DEBUG] TTS produced %zu samples "
                          "@ %u Hz\n",
                          audio.pcm16.size(), audio.sample_rate_hz);
            XPLMDebugString(dbg);
          }
          if (on_playback_starting)
            on_playback_starting();
          int com = com_override > 0 ? com_override : settings::active_com();
          audio_player::play_pcm_on_com(com, std::move(audio.pcm16),
                                        audio.sample_rate_hz, audio.channels,
                                        settings::volume());
        } else {
          XPLMDebugString(
              "[xp_wellys_atc][ERROR] TTS failed, skipping playback\n");
          state_ = PTTState::IDLE;
        }
      });
}

// Speak a tower response with the state-revert guard active. Used for
// engine output that committed a semantic state transition — if the
// TTS playback fails, the pilot never heard the clearance, so the
// state must be rolled back (or, when a later mutation makes the
// rollback unsafe, the clearance text must remain available for
// REQUEST_REPEAT replay).
//
//   pre_snap     = atc_state_machine::capture_snapshot() taken BEFORE
//                  the process() call that produced `text`.
//   expected_gen = atc_state_machine::current_gen() taken IMMEDIATELY
//                  AFTER that process() call.
//
// On success: nothing else happens — the response plays, state stays.
// On TTS failure: a squelch burst is played on the active COM, then
//   either (a) restore — state rolled back, system entry suggests
//   re-issuing the request, OR (b) stale — a later mutation already
//   bumped gen past expected_gen, the unsent clearance text still
//   lives in last_tower_response_text_, system entry steers the pilot
//   toward "say again" so REQUEST_REPEAT replays it.
static void speak_response_guarded(const std::string &text,
                                   model_manifest::VoiceRole role,
                                   float length_scale,
                                   atc_state_machine::AtcStateSnapshot pre_snap,
                                   uint64_t expected_gen) {
  state_ = PTTState::PLAYING;
  tts_pending_ = true;
  ++total_inferences_;

  std::string final_text = expand_navfix_names(expand_runways(text));

  backends::tts::synthesize_async(
      final_text, role, length_scale,
      [pre_snap = std::move(pre_snap), expected_gen](backends::tts::Audio audio,
                                                     bool success) mutable {
        tts_pending_ = false;
        if (success && !audio.pcm16.empty()) {
          if (settings::debug_logging()) {
            char dbg[160];
            std::snprintf(dbg, sizeof(dbg),
                          "[xp_wellys_atc][DEBUG] TTS produced %zu samples "
                          "@ %u Hz\n",
                          audio.pcm16.size(), audio.sample_rate_hz);
            XPLMDebugString(dbg);
          }
          int com = settings::active_com();
          audio_player::play_pcm_on_com(com, std::move(audio.pcm16),
                                        audio.sample_rate_hz, audio.channels,
                                        settings::volume());
          return;
        }
        // TTS failed — engage the revert guard.
        XPLMDebugString("[xp_wellys_atc][ERROR] TTS failed, applying revert "
                        "guard (squelch + state check)\n");
        const int com = settings::active_com();
        audio_player::play_squelch_burst(com);

        const bool restored =
            atc_state_machine::restore_snapshot_if_gen(pre_snap, expected_gen);
        const char *sys_text =
            restored ? "Radio failure - please repeat your transmission"
                     : "Radio failure - say 'say again' for the missed "
                       "instruction";
        push_transcript(TranscriptEntry{
            static_cast<double>(XPLMGetElapsedTime()),
            TranscriptKind::System,
            sys_text,
            "",
            "",
        });
        state_ = PTTState::IDLE;
      });
}

// Shared "got a pilot transcript, run it through the engine and speak the
// reply" path. Used by:
//   - the STT callback in on_ptt_released() — voice path; quality comes
//     from the STT result and may be < 0.3 (triggers the engine's
//     "say again" branch without writing a pilot transcript row).
//   - submit_text() — Debug-Texteingabe; quality is always 1.0.
//
// Pre-conditions: state_ == PTTState::PROCESSING. ++total_transcriptions_
// has been incremented by the caller.
static void dispatch_pilot_transcript(const std::string &text, float quality) {
  ++total_inferences_;

  const auto &ctx = xplane_context::get();
  float active_freq =
      (ctx.active_com == 1) ? ctx.com1_freq_mhz : ctx.com2_freq_mhz;
  char freq_str[16];
  std::snprintf(freq_str, sizeof(freq_str), "%.3f", active_freq);

  // Transcript writing is a UI concern, done here before handing off to
  // the engine. Low-quality transcripts skip the pilot row and go
  // straight to a "say again" response from the engine.
  bool is_pilot_row_written = false;
  if (quality >= 0.3f) {
    push_transcript(TranscriptEntry{
        static_cast<double>(XPLMGetElapsedTime()),
        TranscriptKind::Pilot,
        text,
        freq_str,
        "",
    });
    is_pilot_row_written = true;
  }

  std::string freq_str_copy = freq_str;

  engine::Input in{
      text,
      quality,
      &ctx,
      settings::pilot_callsign(),
      static_cast<double>(XPLMGetElapsedTime()),
  };

  // Snapshot the ATC state BEFORE process() runs so the TTS revert
  // guard can roll it back on synthesis failure. The matching
  // expected_gen is captured inside the engine callback — directly
  // after process() — so a later auto-correction between the synthesise
  // call and the TTS error reply is detected and switches us to the
  // "stale" branch (clearance text stays accessible via REQUEST_REPEAT).
  auto pre_snap = atc_state_machine::capture_snapshot();

  engine::process_transcript(
      std::move(in),
      [freq_str_copy, is_pilot_row_written,
       pre_snap = std::move(pre_snap)](const engine::Output &out) mutable {
        last_pilot_message_ = out.parsed;
        if (out.response_text.empty()) {
          state_ = PTTState::IDLE;
          return;
        }
        // expected_gen lives between process() and TTS callback —
        // captured here, the instant the engine returns.
        const uint64_t expected_gen = atc_state_machine::current_gen();
        // Quality-rejection path didn't write a pilot row — the ATC
        // "say again" still deserves a transcript entry with the active
        // frequency.
        std::string freq_for_atc =
            is_pilot_row_written ? freq_str_copy : std::string();
        push_transcript(TranscriptEntry{
            static_cast<double>(XPLMGetElapsedTime()),
            TranscriptKind::Tower,
            out.response_text,
            freq_for_atc,
            current_tower_label(),
        });
        // Role follows the frequency the pilot is currently tuned to —
        // that's the controller actually transmitting. Tying it to the
        // state machine misroutes handoff messages (Ground saying
        // "contact Tower" would speak with Tower's voice).
        const auto &c = xplane_context::get();
        auto role = role_for_frequency(c);
        speak_response_guarded(out.response_text, role, 1.0f,
                               std::move(pre_snap), expected_gen);
      });
}

void init() {
  state_ = PTTState::IDLE;
  tts_pending_ = false;
  last_duration_ = 0.0f;
  last_samples_ = 0;
  last_wav_bytes_ = 0;
  transcript_.clear();
  last_pilot_message_ = {};
  // (Re-)open the transcript log — truncate so each session starts fresh.
  if (g_transcript_log_) {
    std::fclose(g_transcript_log_);
    g_transcript_log_ = nullptr;
  }
  std::string log_path =
      model_paths::plugin_root() + "/Resources/transcript.log";
  g_transcript_log_ = std::fopen(log_path.c_str(), "w");
  if (g_transcript_log_) {
    logging::info("Transcript log: %s", log_path.c_str());
    g_last_stt_model_.clear(); // force header on first transcription
    write_stt_header_if_changed();
  }
  total_transcriptions_ = 0;
  total_inferences_ = 0;
  engine::reset();
  atis_playing_ = false;
  atis_active_com_ = 0;
  atis_cooldown_ = 0.0f;
  atis_tuned_timer_ = 0.0f;
}

void stop() {
  state_ = PTTState::IDLE;
  tts_pending_ = false;
  if (g_transcript_log_) {
    std::fclose(g_transcript_log_);
    g_transcript_log_ = nullptr;
  }
}

void on_ptt_pressed() {
  if (state_ != PTTState::IDLE) {
    char buf[128];
    std::snprintf(buf, sizeof(buf), "[xp_wellys_atc] PTT blocked, state=%d\n",
                  static_cast<int>(state_));
    XPLMDebugString(buf);
    return;
  }

  // Radio requires power (checks COM radio power DataRef, handles
  // avionics master, battery, and individual radio switches)
  const auto &ctx = xplane_context::get();
  if (!ctx.com_radio_powered) {
    XPLMDebugString("[xp_wellys_atc] PTT blocked - COM radio not powered\n");
    return;
  }

  // Backends must be loaded — without STT we cannot transcribe and
  // without LM we cannot reliably resolve low-confidence transcripts.
  // The plugin's startup path surfaces the model-download dialog when
  // anything is missing; this gate prevents PTT from doing nothing
  // visible.
  if (!backends::stt_ready() || !backends::lm_ready() ||
      !backends::tts_ready()) {
    XPLMDebugString("[xp_wellys_atc][ERROR] PTT blocked - local models not "
                    "loaded (open the plugin window to download)\n");
    return;
  }

  state_ = PTTState::RECORDING;
  audio_player::play_ptt_click();
  audio_recorder::start_recording();
  if (settings::debug_logging())
    XPLMDebugString("[xp_wellys_atc][DEBUG] PTT pressed\n");
}

// Called once the mic is stopped (after the tail expires). Handles the
// minimum-duration gate, takes the PCM buffer, and dispatches to STT.
static void submit_recording_to_stt() {
  last_duration_ = audio_recorder::duration_seconds();
  last_samples_ = audio_recorder::buffer_samples();

  if (last_duration_ < kMinRecordingDuration) {
    char buf[128];
    std::snprintf(buf, sizeof(buf),
                  "[xp_wellys_atc] Recording too short (%.2fs), discarding\n",
                  last_duration_);
    XPLMDebugString(buf);
    state_ = PTTState::IDLE;
    return;
  }

  std::vector<int16_t> pcm = audio_recorder::take_pcm();
  unsigned src_rate = audio_recorder::sample_rate_hz();
  last_wav_bytes_ = pcm.size() * sizeof(int16_t);

  if (settings::debug_logging()) {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "[xp_wellys_atc][DEBUG] Recording stopped: %.1fs, %zu "
                  "samples @ %u Hz\n",
                  last_duration_, last_samples_, src_rate);
    XPLMDebugString(buf);
  }

  state_ = PTTState::PROCESSING;

  // Build the STT initial-prompt context — biases transcription of
  // local proper nouns ("Grenchen", "Speck") AND of the pilot's own
  // phonetic callsign (long NATO sequences like "November One Two
  // Three Alpha Bravo" are otherwise a frequent mishear source —
  // problem #4 in the planning doc). The string is consumed three
  // different ways depending on backend:
  //   - whisper_stt → whisper_full_params.initial_prompt (freeform)
  //   - openai_stt  → "prompt" multipart field (freeform)
  //   - mistral_stt → context_bias[] (split on whitespace; more
  //                   tokens = more bias entries)
  // — so adding the callsign tokens here biases all three.
  // Start with the static ATC vocabulary prompt (full NATO phonetic alphabet
  // + common ATC phrases from atc_prompt_templates.json). This forms the
  // foundation for all three STT backends:
  //   - whisper_stt  → whisper_full_params.initial_prompt
  //   - openai_stt   → "prompt" multipart field
  //   - mistral_stt  → split on whitespace/commas → context_bias[] entries
  // Dynamic tokens appended below further anchor the specific flight.
  const auto &ctx_for_whisper = xplane_context::get();

  std::string airport_ctx = atc_templates::get_prompt("whisper_prompt");
  if (!airport_ctx.empty())
    airport_ctx += " ";
  airport_ctx += ctx_for_whisper.nearest_airport_id;
  if (!ctx_for_whisper.nearest_airport_name.empty())
    airport_ctx += " " + ctx_for_whisper.nearest_airport_name;
  // Aircraft registration (e.g. "N111RC", "F-HABC") from X-Plane's acf_tailnum
  // DataRef — anchors the short-form tail number the pilot uses in radio calls.
  if (!ctx_for_whisper.aircraft_tail_number.empty())
    airport_ctx += " " + ctx_for_whisper.aircraft_tail_number;
  const std::string &locked_rwy = atc_state_machine::assigned_runway();
  if (!locked_rwy.empty())
    airport_ctx += " runway " + locked_rwy;
  // Prefer the locked session callsign once Tower has accepted one —
  // that's the exact phrasing the controller will use back. Before
  // the lock fires, fall back to the user's configured phonetic
  // expansion. Always also append the raw tail-number form (e.g.
  // "N123AB") so single-token utterances like "N-one-two-three" stay
  // anchored even when Whisper drops the phonetic spelling.
  const std::string &sess_cs = atc_state_machine::session_callsign();
  const std::string phonetic =
      !sess_cs.empty() ? sess_cs : settings::pilot_callsign();
  if (!phonetic.empty())
    airport_ctx += " " + phonetic;
  const std::string raw_cs = settings::pilot_callsign_raw();
  if (!raw_cs.empty())
    airport_ctx += " " + raw_cs;
  // SID, STAR, destination ICAO + name, and all FPL fix idents from SimBrief OFP.
  // Built fresh every PTT so the STAR name appears as soon as ATC assigns it.
  // Cap at 60 fixes to avoid inflating the prompt on long-haul routes.
  {
    const auto &ofp = simbrief_ofp::get();
    // Destination: ICAO code + apt.dat name so the pilot's readback is
    // recognised regardless of whether they say "LFSR" or "Reims-Prunay".
    if (!ofp.destination_icao.empty())
      airport_ctx += " " + ofp.destination_icao;
    if (!ctx_for_whisper.ifr_destination.empty())
      airport_ctx += " " + ctx_for_whisper.ifr_destination;
    if (!ofp.sid_name.empty())
      airport_ctx += " " + ofp.sid_name;
    const std::string &star = engine::assigned_star_name();
    if (!star.empty())
      airport_ctx += " " + star;
    int fix_count = 0;
    for (const auto &fix : ofp.navlog) {
      if (!fix.ident.empty() && fix_count < 60) {
        airport_ctx += " " + fix.ident;
        ++fix_count;
      }
    }
    // Destination arrival controller (Approach or Information/FIS).
    // Try TRACON first (proper Approach); fall back to CTR which is how
    // XP12 atc.dat encodes FIS/Information services (e.g. "REIMS" for LFSR).
    if (!ofp.destination_icao.empty()) {
      const auto dest_pos =
          xplane_context::airport_pos_for(ofp.destination_icao);
      if (dest_pos.first != 0.0 || dest_pos.second != 0.0) {
        const airspace_db::Controller *arr_ctrl =
            airspace_db::find_by_role_near(airspace_db::ControllerRole::TRACON,
                                           dest_pos.first, dest_pos.second, 0);
        if (!arr_ctrl)
          arr_ctrl = airspace_db::find_by_role_near(
              airspace_db::ControllerRole::CTR, dest_pos.first,
              dest_pos.second, 0);
        if (arr_ctrl && !arr_ctrl->name.empty()) {
          // Raw atc.dat name tokens (e.g. "REIMS" "INFORMATION") — case-insensitive coverage.
          std::istringstream iss(arr_ctrl->name);
          std::string tok;
          while (iss >> tok)
            airport_ctx += " " + tok;
          // Human-readable city from the controller's facility airport (e.g. LFSM → "Reims"),
          // pre-loaded before the handoff fires so the readback is biased from the first PTT.
          if (!arr_ctrl->facility_id.empty()) {
            const std::string apt =
                xplane_context::airport_name_for(arr_ctrl->facility_id);
            if (!apt.empty()) {
              auto sp = apt.find(' ');
              std::string city =
                  (sp == std::string::npos) ? apt : apt.substr(0, sp);
              if (!city.empty()) {
                city[0] = static_cast<char>(
                    std::toupper(static_cast<unsigned char>(city[0])));
                for (std::size_t i = 1; i < city.size(); ++i)
                  city[i] = static_cast<char>(
                      std::tolower(static_cast<unsigned char>(city[i])));
                airport_ctx += " " + city;
              }
            }
          }
        }
      }
    }
  }

  // Add departure controller name so Voxtral recognises French/German city
  // names in pilot readbacks (e.g. "Chambery" → not "chamber",
  // "Marseille" → correct, "Strasbourg" → not "Strasbourg Control").
  // Use the pending label (set at takeoff clearance time) if the active
  // label has not yet been activated by poll_departure_handoff().
  {
    const std::string &dep_label = engine::current_controller_label().empty()
                                       ? engine::pending_departure_label()
                                       : engine::current_controller_label();
    if (!dep_label.empty()) {
      std::istringstream iss(dep_label);
      std::string tok;
      while (iss >> tok)
        airport_ctx += " " + tok;
    }
  }

  // Add the pending handoff frequency and all known airport frequencies as
  // numeric tokens (e.g. "120.230").  When these are in context_bias, Voxtral
  // outputs them as digits directly ("120.230") rather than phonetic words
  // ("one to zero decimal two three zero"), making frequency readbacks reliable.
  {
    char freq_buf[16];
    for (const auto &af : ctx_for_whisper.airport_freqs.all) {
      if (af.freq_khz > 0) {
        std::snprintf(freq_buf, sizeof(freq_buf), "%.3f",
                      static_cast<float>(af.freq_khz) / 1000.0f);
        airport_ctx += " ";
        airport_ctx += freq_buf;
      }
    }
    const float ph = engine::pending_handoff_freq();
    if (ph > 0.0f) {
      std::snprintf(freq_buf, sizeof(freq_buf), "%.3f", ph);
      airport_ctx += " ";
      airport_ctx += freq_buf;
    }
  }

  // Extra individual words not in the static whisper_prompt template —
  // these are the specific Voxtral/WhisperATC mishearing hotspots
  // identified in flight testing (see project_voxtral_stt_errors.md).
  // Each token becomes a separate context_bias[] entry for Voxtral and
  // extends the initial_prompt for whisper.cpp / OpenAI.
  airport_ctx +=
      " Romeo IFR QNH Mode Charlie holding point squawk vacated"
      " taxi RNAV report ground tower wilco roger startup"
      " climb climbing departure ready"
      " filed maintain cleared descend identified request"
      " contact frequency approach control radar information centre"
      " cancelling cancel cancellation";

  if (g_transcript_log_) {
    static std::string s_last_logged_ctx;
    if (airport_ctx != s_last_logged_ctx) {
      std::fprintf(g_transcript_log_, "-- CTX: %s --\n", airport_ctx.c_str());
      s_last_logged_ctx = airport_ctx;
    } else {
      std::fprintf(g_transcript_log_, "-- CTX: (unchanged) --\n");
    }
    std::fflush(g_transcript_log_);
  }

  backends::stt::transcribe_async(
      std::move(pcm), src_rate,
      [](const backends::stt::TranscriptResult &wr) {
        if (!wr.success) {
          // Prefer the structured error_message (set by the cloud
          // backends via openai_common::interpret) so the user sees
          // the actual cause ("STT-OPENAI: Operation timed out").
          // Fall back to wr.text for older callers that still pack
          // the human-readable message there.
          const std::string display =
              !wr.error_message.empty() ? wr.error_message : wr.text;
          logging::error("STT error: %s", display.c_str());
          push_transcript(TranscriptEntry{
              static_cast<double>(XPLMGetElapsedTime()),
              TranscriptKind::System,
              display,
              "",
              "",
          });
          state_ = PTTState::IDLE;
          return;
        }

        ++total_transcriptions_;
        dispatch_pilot_transcript(wr.text, wr.quality);
      },
      airport_ctx);
}

void on_ptt_released() {
  if (state_ != PTTState::RECORDING)
    return;
  // Keep the mic open for kPttTailSec so the last syllable is captured
  // before the buffer is handed to STT.
  state_ = PTTState::TAIL_RECORDING;
  ptt_tail_remaining_ = kPttTailSec;
}

// Debug-Texteingabe convenience: replace the keyword "REG" (case-
// insensitive) with the configured phonetic pilot callsign. Lets the
// user type "Bern Tower REG, ready for departure" instead of spelling
// "November One Two Three Alpha Bravo" every time. Voice path is
// untouched — Whisper produces phonetic words, this expansion only
// runs for typed input.
//
// Word-boundary matching: the keyword must be its own token (after
// stripping surrounding punctuation), so "REGION" stays unchanged.
// All occurrences in the text are replaced.
static std::string expand_callsign_placeholder(const std::string &text) {
  const std::string phonetic = settings::pilot_callsign();
  if (phonetic.empty())
    return text;

  auto eq_ci = [](const std::string &a, const char *b) {
    size_t i = 0;
    for (; i < a.size() && b[i] != '\0'; ++i) {
      if (std::tolower(static_cast<unsigned char>(a[i])) !=
          std::tolower(static_cast<unsigned char>(b[i])))
        return false;
    }
    return i == a.size() && b[i] == '\0';
  };

  std::string out;
  out.reserve(text.size() + phonetic.size());
  size_t i = 0;
  const size_t n = text.size();
  while (i < n) {
    if (std::isspace(static_cast<unsigned char>(text[i]))) {
      out += text[i++];
      continue;
    }
    size_t tok_start = i;
    while (i < n && !std::isspace(static_cast<unsigned char>(text[i])))
      ++i;
    std::string tok = text.substr(tok_start, i - tok_start);

    // Split off leading and trailing punctuation around the core.
    size_t lead = 0;
    while (lead < tok.size() &&
           !std::isalnum(static_cast<unsigned char>(tok[lead])))
      ++lead;
    size_t trail = tok.size();
    while (trail > lead &&
           !std::isalnum(static_cast<unsigned char>(tok[trail - 1])))
      --trail;
    std::string core = tok.substr(lead, trail - lead);
    if (eq_ci(core, "REG")) {
      out += tok.substr(0, lead);
      out += phonetic;
      out += tok.substr(trail);
    } else {
      out += tok;
    }
  }
  return out;
}

void submit_text(const std::string &text) {
  if (state_ != PTTState::IDLE) {
    char buf[128];
    std::snprintf(buf, sizeof(buf),
                  "[xp_wellys_atc] submit_text blocked, state=%d\n",
                  static_cast<int>(state_));
    XPLMDebugString(buf);
    return;
  }
  if (text.empty())
    return;
  // LM + TTS still need to be loaded — text bypasses STT, but the
  // downstream stages are identical to the voice path.
  if (!backends::lm_ready() || !backends::tts_ready()) {
    XPLMDebugString("[xp_wellys_atc][ERROR] submit_text blocked - LM/TTS not "
                    "ready (open the plugin window to download)\n");
    return;
  }
  state_ = PTTState::PROCESSING;
  ++total_transcriptions_;
  // quality = 1.0 — typed text skips the Whisper quality gate; the engine
  // will run the full intent-parse + state-machine path with no "say
  // again" short-circuit. Expand the "REG" placeholder keyword to the
  // configured phonetic callsign so the user doesn't have to spell out
  // the full registration in every test message.
  dispatch_pilot_transcript(expand_callsign_placeholder(text), 1.0f);
}

void update() {
  if (state_ == PTTState::PLAYING && !tts_pending_ &&
      !audio_player::is_playing()) {
    if (atis_playing_) {
      atis_playing_ = false;
      if (settings::debug_logging())
        XPLMDebugString(
            "[xp_wellys_atc][DEBUG] ATIS playback finished, state -> IDLE\n");
    } else {
      if (settings::debug_logging())
        XPLMDebugString(
            "[xp_wellys_atc][DEBUG] Playback finished, state -> IDLE\n");
    }
    state_ = PTTState::IDLE;
  }

  // PTT tail: keep recording for kPttTailSec after key release, then submit.
  float dt = 1.0f / 60.0f; // approximate per-frame at ~60fps
  if (state_ == PTTState::TAIL_RECORDING) {
    ptt_tail_remaining_ -= dt;
    if (ptt_tail_remaining_ <= 0.0f) {
      audio_recorder::stop_recording();
      state_ = PTTState::PROCESSING;
      submit_recording_to_stt();
    }
  }

  // ATIS cooldown timer
  if (atis_cooldown_ > 0.0f)
    atis_cooldown_ -= dt;

  // Flight-phase auto-correction of ATC state
  double now_secs_for_state = static_cast<double>(XPLMGetElapsedTime());
  atc_state_machine::check_auto_correction(flight_phase::get(), dt,
                                           now_secs_for_state);

  // Airport-change reset of EN_ROUTE → IDLE. Runs every frame so the
  // UI hint pipeline reflects a new airport's options the moment the
  // airport lock changes, instead of waiting for the next PTT call.
  atc_state_machine::check_airport_change(xplane_context::get(),
                                          now_secs_for_state);

  // Route fix tracker — logging only, no audio. Runs every frame regardless
  // of PTT state. Writes a System entry to transcript + Log.txt when the
  // aircraft enters the 5 NM zone around the next fix on the route.
  {
    const auto &ctx_rft = xplane_context::get();
    const std::string track_event = engine::poll_route_tracker(ctx_rft);
    if (!track_event.empty()) {
      push_transcript(TranscriptEntry{
          static_cast<double>(XPLMGetElapsedTime()),
          TranscriptKind::System,
          track_event,
          {},
          {},
      });
    }
  }

  // Phase-2 traffic advisory poll. SDK-free engine helper consumes the
  // live traffic_context snapshot + state-machine state and may emit a
  // synthetic advisory transition. Only run while idle so a controller
  // utterance never overlaps with a pilot-driven exchange.
  if (state_ == PTTState::IDLE && backends::tts_ready()) {
    const auto &ctx_now = xplane_context::get();
    double now_secs = static_cast<double>(XPLMGetElapsedTime());

    // Ground runway-change notification: fires before any other poll so the
    // pilot is warned immediately when the active runway changes on the ground.
    std::string runway_change_text;
    if (engine::poll_ground_runway_change(ctx_now, &runway_change_text) &&
        !runway_change_text.empty()) {
      float active_freq = (ctx_now.active_com == 1) ? ctx_now.com1_freq_mhz
                                                    : ctx_now.com2_freq_mhz;
      char freq_str[16];
      std::snprintf(freq_str, sizeof(freq_str), "%.3f", active_freq);
      push_transcript(TranscriptEntry{
          static_cast<double>(XPLMGetElapsedTime()),
          TranscriptKind::Tower,
          runway_change_text,
          freq_str,
          current_tower_label(),
      });
      auto role = role_for_frequency(ctx_now);
      speak_response(runway_change_text, role, 1.0f);
      return; // one tower utterance per frame
    }

    // Readback-reminder poll runs FIRST (among pilot-dialog polls). The pilot was supposed to
    // read a clearance back and went silent — the tower nudge takes
    // precedence over traffic advisories and even over an unsolicited
    // go-around (which can't happen anyway without an active
    // clearance in PROCESSING). Cancellation case is handled inside
    // atc_state_machine before the call returns; here we just speak.
    std::string readback_reminder_text;
    if (engine::poll_readback_reminder(ctx_now, now_secs,
                                       &readback_reminder_text) &&
        !readback_reminder_text.empty()) {
      float active_freq = (ctx_now.active_com == 1) ? ctx_now.com1_freq_mhz
                                                    : ctx_now.com2_freq_mhz;
      char freq_str[16];
      std::snprintf(freq_str, sizeof(freq_str), "%.3f", active_freq);
      push_transcript(TranscriptEntry{
          static_cast<double>(XPLMGetElapsedTime()),
          TranscriptKind::Tower,
          readback_reminder_text,
          freq_str,
          current_tower_label(),
      });
      auto role = role_for_frequency(ctx_now);
      speak_response(readback_reminder_text, role, 1.0f);
      return; // one tower utterance per frame
    }

    // IFR departure handoff: Tower tells the pilot to contact Departure or
    // Approach ~10 s into climb. Fires before go-around/traffic checks.
    // Capture label before poll_departure_handoff() updates
    // s_current_controller_label.
    std::string label_pre_handoff = current_tower_label();
    std::string departure_handoff_text;
    if (engine::poll_departure_handoff(ctx_now, dt, &departure_handoff_text) &&
        !departure_handoff_text.empty()) {
      float active_freq = (ctx_now.active_com == 1) ? ctx_now.com1_freq_mhz
                                                    : ctx_now.com2_freq_mhz;
      char freq_str[16];
      std::snprintf(freq_str, sizeof(freq_str), "%.3f", active_freq);
      push_transcript(TranscriptEntry{
          static_cast<double>(XPLMGetElapsedTime()),
          TranscriptKind::Tower,
          departure_handoff_text,
          freq_str,
          label_pre_handoff,
      });
      auto role = role_for_frequency(ctx_now);
      speak_response(departure_handoff_text, role, 1.0f);
      return; // one tower utterance per frame
    }

    // IFR SID climb management: ATC-initiated step climbs and direct-to
    // shortcut while in IFR_RADAR_CONTACT state.
    // Capture label before poll_sid_climb() may update
    // s_current_controller_label (TMA exit).
    std::string label_pre_climb = current_tower_label();
    std::string sid_climb_text;
    if (engine::poll_sid_climb(ctx_now, dt, &sid_climb_text) &&
        !sid_climb_text.empty()) {
      float active_freq = (ctx_now.active_com == 1) ? ctx_now.com1_freq_mhz
                                                    : ctx_now.com2_freq_mhz;
      char freq_str[16];
      std::snprintf(freq_str, sizeof(freq_str), "%.3f", active_freq);
      push_transcript(TranscriptEntry{
          static_cast<double>(XPLMGetElapsedTime()),
          TranscriptKind::Tower,
          sid_climb_text,
          freq_str,
          label_pre_climb,
      });
      auto role = role_for_frequency(ctx_now);
      speak_response(sid_climb_text, role, 1.0f);
      return; // one tower utterance per frame
    }

    // IFR en-route management: Centre direct-to shortcut, TMA entry descent
    // clearance (proactive — ATC does not wait for pilot request), and
    // cross-track deviation alert.
    std::string enroute_text;
    std::string label_pre_enroute = current_tower_label();
    bool enroute_rb = false;
    if (engine::poll_enroute(ctx_now, dt, &enroute_text, &enroute_rb) &&
        !enroute_text.empty()) {
      float active_freq = (ctx_now.active_com == 1) ? ctx_now.com1_freq_mhz
                                                    : ctx_now.com2_freq_mhz;
      char freq_str[16];
      std::snprintf(freq_str, sizeof(freq_str), "%.3f", active_freq);
      push_transcript(TranscriptEntry{
          static_cast<double>(XPLMGetElapsedTime()),
          TranscriptKind::Tower,
          enroute_text,
          freq_str,
          label_pre_enroute,
      });
      auto role = role_for_frequency(ctx_now);
      speak_response(enroute_text, role, 1.0f);
      if (enroute_rb)
        atc_state_machine::arm_readback(enroute_text);
      return; // one tower utterance per frame
    }

    // IFR approach STAR constraint management: step-down clearances + final alt.
    std::string approach_text;
    bool approach_rb = false;
    if (engine::poll_approach(ctx_now, dt, &approach_text, &approach_rb) &&
        !approach_text.empty()) {
      float active_freq = (ctx_now.active_com == 1) ? ctx_now.com1_freq_mhz
                                                    : ctx_now.com2_freq_mhz;
      char freq_str[16];
      std::snprintf(freq_str, sizeof(freq_str), "%.3f", active_freq);
      push_transcript(TranscriptEntry{
          static_cast<double>(XPLMGetElapsedTime()),
          TranscriptKind::Tower,
          approach_text,
          freq_str,
          engine::current_controller_label(),
      });
      auto role = role_for_frequency(ctx_now);
      speak_response(approach_text, role, 1.0f);
      if (approach_rb)
        atc_state_machine::arm_readback(approach_text);
      return; // one utterance per frame
    }

    // After-FAF lateral deviation: "confirm established on the approach".
    std::string alignment_text;
    if (engine::poll_approach_alignment(ctx_now, dt, &alignment_text) &&
        !alignment_text.empty()) {
      float active_freq = (ctx_now.active_com == 1) ? ctx_now.com1_freq_mhz
                                                    : ctx_now.com2_freq_mhz;
      char freq_str[16];
      std::snprintf(freq_str, sizeof(freq_str), "%.3f", active_freq);
      push_transcript(TranscriptEntry{
          static_cast<double>(XPLMGetElapsedTime()),
          TranscriptKind::Tower,
          alignment_text,
          freq_str,
          engine::current_controller_label(),
      });
      auto role = role_for_frequency(ctx_now);
      speak_response(alignment_text, role, 1.0f);
      return;
    }

    // Phase-4 go-around trigger runs *before* the traffic advisory so a
    // single tick can never produce both: when the runway is occupied,
    // the go-around call is the more urgent of the two.
    std::string go_around_text;
    if (engine::poll_go_around(ctx_now, now_secs, &go_around_text) &&
        !go_around_text.empty()) {
      float active_freq = (ctx_now.active_com == 1) ? ctx_now.com1_freq_mhz
                                                    : ctx_now.com2_freq_mhz;
      char freq_str[16];
      std::snprintf(freq_str, sizeof(freq_str), "%.3f", active_freq);
      push_transcript(TranscriptEntry{
          static_cast<double>(XPLMGetElapsedTime()),
          TranscriptKind::Tower,
          go_around_text,
          freq_str,
          current_tower_label(),
      });
      auto role = role_for_frequency(ctx_now);
      speak_response(go_around_text, role, 1.0f);
    } else {
      std::string advisory_text;
      if (engine::poll_traffic_advisory(ctx_now, now_secs, &advisory_text) &&
          !advisory_text.empty()) {
        float active_freq = (ctx_now.active_com == 1) ? ctx_now.com1_freq_mhz
                                                      : ctx_now.com2_freq_mhz;
        char freq_str[16];
        std::snprintf(freq_str, sizeof(freq_str), "%.3f", active_freq);
        push_transcript(TranscriptEntry{
            static_cast<double>(XPLMGetElapsedTime()),
            TranscriptKind::Tower,
            advisory_text,
            freq_str,
            current_tower_label(),
        });
        auto role = role_for_frequency(ctx_now);
        speak_response(advisory_text, role, 1.0f);
      }
    }
  }

  // Airport-change detection lives in atc_state_machine::process now —
  // it fires off the next pilot transmission. No per-frame loop here.

  // ATIS playback trigger — requires COM radio power + tuning delay.
  // ATIS reception works on EITHER COM1 or COM2: pilots commonly park
  // ATIS on COM2 (standby) while keeping COM1 on the controller's freq.
  const auto &ctx = xplane_context::get();
  int atis_com =
      ctx.com_radio_powered ? atis_generator::which_com_tuned_to_atis(ctx) : 0;
  bool tuned = atis_com != 0;

  if (tuned) {
    atis_tuned_timer_ += dt;
  } else {
    atis_tuned_timer_ = 0.0f;
  }

  // Pilot retuned (or powered off) the specific COM that's playing ATIS
  // — abort. Aborts even if the OTHER COM is still on ATIS, because we
  // can't switch buses mid-broadcast. The cooldown stays intact so re-
  // tuning ATIS within the cooldown window stays silent (we already
  // announced this letter).
  if (atis_playing_ && atis_com != atis_active_com_) {
    audio_player::abort_playback();
    atis_playing_ = false;
    state_ = PTTState::IDLE;
    if (settings::debug_logging())
      XPLMDebugString("[xp_wellys_atc][DEBUG] ATIS aborted: pilot retuned "
                      "the COM that was playing ATIS\n");
  }

  // ATIS is a side-channel like Traffic — independent of ATCState.
  // Pilot can re-tune ATIS at any point (e.g. holding point) to refresh
  // the broadcast. Only PTT state and TTS readiness gate playback so we
  // never overlap an active pilot/controller exchange.
  if (state_ == PTTState::IDLE && atis_cooldown_ <= 0.0f && tuned &&
      atis_tuned_timer_ >= kAtisTuneDelaySec && backends::tts_ready()) {
    std::string atis_text = atis_generator::generate_atis_text(ctx);
    if (atis_text.empty())
      return; // no ATIS letter assigned yet -- skip playback this frame

    if (settings::debug_logging()) {
      char dbg[64];
      std::snprintf(dbg, sizeof(dbg), " (COM%d)", atis_com);
      XPLMDebugString(("[xp_wellys_atc][DEBUG] ATIS triggered" +
                       std::string(dbg) + ": " + atis_text + "\n")
                          .c_str());
    }

    // Log the ATIS broadcast against the COM it actually plays on, not
    // the active COM — pilot may be on Tower with active=COM1 while
    // ATIS streams through COM2.
    float atis_com_freq =
        (atis_com == 2) ? ctx.com2_freq_mhz : ctx.com1_freq_mhz;
    char freq_str[16];
    std::snprintf(freq_str, sizeof(freq_str), "%.3f", atis_com_freq);

    // Stage the transcript entry but do not publish it yet — the push
    // happens in the TTS success callback so a silent synthesis
    // failure (bad voice id, OpenAI error, network) leaves no ghost
    // line in the history. Cooldown + atis_playing_ ARE set up-front
    // to block a retrigger loop while synthesis is in flight.
    TranscriptEntry pending_entry{
        static_cast<double>(XPLMGetElapsedTime()),
        TranscriptKind::Tower,
        atis_text,
        freq_str,
        current_tower_label(),
    };

    atis_playing_ = true;
    atis_active_com_ = atis_com;
    atis_cooldown_ = kAtisCooldownSec;
    // ATIS reads slower than tower/ground — Piper length_scale > 1
    // produces the slower rate the OpenAI path used to get from
    // speed=0.85.
    speak_response(atis_text, model_manifest::VoiceRole::Atis, 1.18f, atis_com,
                   [entry = std::move(pending_entry)]() mutable {
                     push_transcript(std::move(entry));
                   });
  }
}

void reset_atis_cooldown() {
  atis_cooldown_ = 0.0f;
  atis_tuned_timer_ = 0.0f;
}

PTTState ptt_state() { return state_; }

std::string ptt_state_label() {
  switch (state_) {
  case PTTState::IDLE:
    return "Ready";
  case PTTState::RECORDING:
  case PTTState::TAIL_RECORDING:
    return "[REC]";
  case PTTState::PROCESSING:
    return "[Processing...]";
  case PTTState::PLAYING:
    return "[ATC speaking...]";
  }
  return "UNKNOWN";
}

float last_recording_duration() { return last_duration_; }
size_t last_recording_samples() { return last_samples_; }
size_t last_wav_bytes() { return last_wav_bytes_; }

const intent_parser::PilotMessage &last_pilot_message() {
  return last_pilot_message_;
}

const std::vector<TranscriptEntry> &transcript_entries() { return transcript_; }

void clear_transcript() { transcript_.clear(); }

std::string last_atc_response() {
  // Tower entries only — System entries (e.g. "Funkstoerung") do not
  // count as ATC replies, even though they appear in the transcript.
  for (auto it = transcript_.rbegin(); it != transcript_.rend(); ++it) {
    if (it->kind == TranscriptKind::Tower)
      return it->text;
  }
  return "";
}

int total_transcriptions() { return total_transcriptions_; }
int total_api_calls() { return total_inferences_ + engine::lm_inferences(); }

} // namespace atc_session
