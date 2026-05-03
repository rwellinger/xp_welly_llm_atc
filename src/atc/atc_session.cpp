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
#include "atc/intent_parser.hpp"
#include "audio/audio_player.hpp"
#include "audio/audio_recorder.hpp"
#include "backends/manager.hpp"
#include "core/logging.hpp"
#include "core/xplane_context.hpp"
#include "persistence/model_manifest.hpp"
#include "persistence/settings.hpp"

#include <XPLMProcessing.h>
#include <XPLMUtilities.h>

#include <cstdio>
#include <string>

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
static int total_transcriptions_ = 0;
static int total_inferences_ = 0;
static constexpr float kMinRecordingDuration = 0.5f;

// ATIS playback state
static bool atis_playing_ = false;
static float atis_cooldown_ = 0.0f;
static constexpr float kAtisCooldownSec = 30.0f;
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
  case FT::UNICOM:
  case FT::CTAF:
  case FT::UNKNOWN:
    return R::Center;
  }
  return R::Center;
}

// Speak ATC response via local TTS, then transition to PLAYING → IDLE.
// `length_scale` > 1.0 makes Piper speak slower (used for ATIS).
static void speak_response(const std::string &text,
                           model_manifest::VoiceRole role,
                           float length_scale = 1.0f) {
  state_ = PTTState::PLAYING;
  tts_pending_ = true;
  ++total_inferences_; // TTS inference

  backends::tts::synthesize_async(
      text, role, length_scale, [](backends::tts::Audio audio, bool success) {
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
          audio_player::play_pcm(std::move(audio.pcm16), audio.sample_rate_hz,
                                 audio.channels, settings::volume());
        } else {
          XPLMDebugString(
              "[xp_wellys_atc][ERROR] TTS failed, skipping playback\n");
          state_ = PTTState::IDLE;
        }
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
  total_transcriptions_ = 0;
  total_inferences_ = 0;
  engine::reset();
  atis_playing_ = false;
  atis_cooldown_ = 0.0f;
  atis_tuned_timer_ = 0.0f;
}

void stop() {
  state_ = PTTState::IDLE;
  tts_pending_ = false;
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

void on_ptt_released() {
  if (state_ != PTTState::RECORDING)
    return;

  audio_recorder::stop_recording();

  last_duration_ = audio_recorder::duration_seconds();
  last_samples_ = audio_recorder::buffer_samples();

  // Minimum duration gate
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

  // Build airport context for the Whisper initial prompt — biases
  // transcription of local proper nouns ("Grenchen", "Speck", etc.).
  const auto &ctx_for_whisper = xplane_context::get();
  std::string airport_ctx = ctx_for_whisper.nearest_airport_id;
  if (!ctx_for_whisper.nearest_airport_name.empty())
    airport_ctx += " " + ctx_for_whisper.nearest_airport_name;

  backends::stt::transcribe_async(
      std::move(pcm), src_rate,
      [](const backends::stt::TranscriptResult &wr) {
        if (!wr.success) {
          logging::error("STT error: %s", wr.text.c_str());
          transcript_.push_back(TranscriptEntry{
              static_cast<double>(XPLMGetElapsedTime()),
              false,
              wr.text,
              "",
          });
          state_ = PTTState::IDLE;
          return;
        }

        ++total_transcriptions_;
        ++total_inferences_;

        const auto &ctx = xplane_context::get();
        float active_freq =
            (ctx.active_com == 1) ? ctx.com1_freq_mhz : ctx.com2_freq_mhz;
        char freq_str[16];
        std::snprintf(freq_str, sizeof(freq_str), "%.3f", active_freq);

        // Transcript writing is a UI concern, done here before handing
        // off to the engine. Low-quality transcripts skip the pilot row
        // and go straight to a "say again" response from the engine.
        bool is_pilot_row_written = false;
        if (wr.quality >= 0.3f) {
          transcript_.push_back(TranscriptEntry{
              static_cast<double>(XPLMGetElapsedTime()),
              true,
              wr.text,
              freq_str,
          });
          is_pilot_row_written = true;
        }

        std::string freq_str_copy = freq_str;

        engine::Input in{
            wr.text,
            wr.quality,
            &ctx,
            settings::pilot_callsign(),
            static_cast<double>(XPLMGetElapsedTime()),
        };

        engine::process_transcript(
            std::move(in),
            [freq_str_copy, is_pilot_row_written](const engine::Output &out) {
              last_pilot_message_ = out.parsed;
              if (out.response_text.empty()) {
                state_ = PTTState::IDLE;
                return;
              }
              // Quality-rejection path didn't write a pilot row — the
              // ATC "say again" still deserves a transcript entry with
              // the active frequency.
              std::string freq_for_atc =
                  is_pilot_row_written ? freq_str_copy : std::string();
              transcript_.push_back(TranscriptEntry{
                  static_cast<double>(XPLMGetElapsedTime()),
                  false,
                  out.response_text,
                  freq_for_atc,
              });
              // Role follows the frequency the pilot is currently
              // tuned to — that's the controller actually transmitting.
              // Tying it to the state machine misroutes handoff
              // messages (Ground saying "contact Tower" would speak
              // with Tower's voice).
              const auto &c = xplane_context::get();
              auto role = role_for_frequency(c);
              speak_response(out.response_text, role, 1.0f);
            });
      },
      airport_ctx);
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

  // ATIS cooldown timer
  float dt = 1.0f / 60.0f; // approximate per-frame at ~60fps
  if (atis_cooldown_ > 0.0f)
    atis_cooldown_ -= dt;

  // Flight-phase auto-correction of ATC state
  atc_state_machine::check_auto_correction(flight_phase::get(), dt);

  // Airport-change reset of EN_ROUTE → IDLE. Runs every frame so the
  // UI hint pipeline reflects a new airport's options the moment the
  // airport lock changes, instead of waiting for the next PTT call.
  atc_state_machine::check_airport_change(xplane_context::get());

  // Phase-2 traffic advisory poll. SDK-free engine helper consumes the
  // live traffic_context snapshot + state-machine state and may emit a
  // synthetic advisory transition. Only run while idle so a controller
  // utterance never overlaps with a pilot-driven exchange.
  if (state_ == PTTState::IDLE && backends::tts_ready()) {
    const auto &ctx_now = xplane_context::get();
    std::string advisory_text;
    double now_secs = static_cast<double>(XPLMGetElapsedTime());
    if (engine::poll_traffic_advisory(ctx_now, now_secs, &advisory_text) &&
        !advisory_text.empty()) {
      float active_freq = (ctx_now.active_com == 1) ? ctx_now.com1_freq_mhz
                                                    : ctx_now.com2_freq_mhz;
      char freq_str[16];
      std::snprintf(freq_str, sizeof(freq_str), "%.3f", active_freq);
      transcript_.push_back(TranscriptEntry{
          static_cast<double>(XPLMGetElapsedTime()),
          false,
          advisory_text,
          freq_str,
      });
      auto role = role_for_frequency(ctx_now);
      speak_response(advisory_text, role, 1.0f);
    }
  }

  // Airport-change detection lives in atc_state_machine::process now —
  // it fires off the next pilot transmission. No per-frame loop here.

  // ATIS playback trigger — requires COM radio power + tuning delay
  const auto &ctx = xplane_context::get();
  bool tuned = ctx.com_radio_powered && atis_generator::is_tuned_to_atis(ctx);

  if (tuned) {
    atis_tuned_timer_ += dt;
  } else {
    atis_tuned_timer_ = 0.0f;
  }

  // ATIS is a side-channel like Traffic — independent of ATCState.
  // Pilot can re-tune ATIS at any point (e.g. holding point) to refresh
  // the broadcast. Only PTT state and TTS readiness gate playback so we
  // never overlap an active pilot/controller exchange.
  if (state_ == PTTState::IDLE && atis_cooldown_ <= 0.0f && tuned &&
      atis_tuned_timer_ >= kAtisTuneDelaySec && backends::tts_ready()) {
    std::string atis_text = atis_generator::generate_atis_text(ctx);

    if (settings::debug_logging())
      XPLMDebugString(
          ("[xp_wellys_atc][DEBUG] ATIS triggered: " + atis_text + "\n")
              .c_str());

    float active_freq =
        (ctx.active_com == 1) ? ctx.com1_freq_mhz : ctx.com2_freq_mhz;
    char freq_str[16];
    std::snprintf(freq_str, sizeof(freq_str), "%.3f", active_freq);

    transcript_.push_back(TranscriptEntry{
        static_cast<double>(XPLMGetElapsedTime()),
        false,
        atis_text,
        freq_str,
    });

    atis_playing_ = true;
    atis_cooldown_ = kAtisCooldownSec;
    // ATIS reads slower than tower/ground — Piper length_scale > 1
    // produces the slower rate the OpenAI path used to get from
    // speed=0.85.
    speak_response(atis_text, model_manifest::VoiceRole::Atis, 1.18f);
  }
}

PTTState ptt_state() { return state_; }

std::string ptt_state_label() {
  switch (state_) {
  case PTTState::IDLE:
    return "Ready";
  case PTTState::RECORDING:
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
  for (auto it = transcript_.rbegin(); it != transcript_.rend(); ++it) {
    if (!it->is_pilot)
      return it->text;
  }
  return "";
}

int total_transcriptions() { return total_transcriptions_; }
int total_api_calls() { return total_inferences_ + engine::lm_inferences(); }

} // namespace atc_session
