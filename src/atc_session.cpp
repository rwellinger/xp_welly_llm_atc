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

#include "atc_session.hpp"
#include "atc_state_machine.hpp"
#include "atis_generator.hpp"
#include "audio_player.hpp"
#include "audio_recorder.hpp"
#include "engine/engine.hpp"
#include "flight_phase.hpp"
#include "intent_parser.hpp"
#include "logging.hpp"
#include "openai/tts_client.hpp"
#include "openai/whisper_client.hpp"
#include "settings.hpp"
#include "xplane_context.hpp"

#include <XPLMProcessing.h>
#include <XPLMUtilities.h>

#include <cstdio>
#include <string>

namespace atc_session {

static PTTState state_ = PTTState::IDLE;

static float last_duration_ = 0.0f;
static size_t last_samples_ = 0;
static size_t last_wav_bytes_ = 0;

static std::vector<TranscriptEntry> transcript_;
static intent_parser::PilotMessage last_pilot_message_;
static int total_transcriptions_ = 0;
static int total_api_calls_ = 0;
static constexpr float kMinRecordingDuration = 0.5f;

// ATIS playback state
static bool atis_playing_ = false;
static float atis_cooldown_ = 0.0f;
static constexpr float kAtisCooldownSec = 30.0f;
static float atis_tuned_timer_ = 0.0f;           // how long tuned to ATIS freq
static constexpr float kAtisTuneDelaySec = 2.0f; // wait before playing

// Determine TTS voice based on frequency type
static std::string voice_for_freq(xplane_context::FrequencyType ft) {
  using FT = xplane_context::FrequencyType;
  if (ft == FT::GROUND)
    return settings::tts_voice_ground();
  return settings::tts_voice_tower();
}

// Speak ATC response via TTS, then transition to PLAYING → IDLE
static void speak_response(const std::string &text, float speed = 1.0f,
                           const std::string &voice = "") {
  state_ = PTTState::PLAYING;
  ++total_api_calls_; // TTS call

  tts_client::speak_async(
      text,
      [](const std::vector<uint8_t> &mp3_data, bool success) {
        if (success && !mp3_data.empty()) {
          if (settings::debug_logging()) {
            char dbg[128];
            std::snprintf(
                dbg, sizeof(dbg),
                "[xp_wellys_atc][DEBUG] TTS response: %zu bytes MP3\n",
                mp3_data.size());
            XPLMDebugString(dbg);
          }
          audio_player::play(mp3_data, settings::volume());
        } else {
          XPLMDebugString(
              "[xp_wellys_atc][ERROR] TTS failed, skipping playback\n");
          state_ = PTTState::IDLE;
        }
      },
      speed, voice);
}

void init() {
  state_ = PTTState::IDLE;
  last_duration_ = 0.0f;
  last_samples_ = 0;
  last_wav_bytes_ = 0;
  transcript_.clear();
  last_pilot_message_ = {};
  total_transcriptions_ = 0;
  total_api_calls_ = 0;
  engine::reset();
  atis_playing_ = false;
  atis_cooldown_ = 0.0f;
  atis_tuned_timer_ = 0.0f;
}

void stop() { state_ = PTTState::IDLE; }

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
    XPLMDebugString("[xp_wellys_atc] PTT blocked — COM radio not powered\n");
    return;
  }

  // API key required
  if (settings::get_api_key().empty()) {
    XPLMDebugString(
        "[xp_wellys_atc][ERROR] PTT blocked — no API key configured\n");
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

  auto wav = audio_recorder::encode_wav();
  last_wav_bytes_ = wav.size();

  if (settings::debug_logging()) {
    char buf[256];
    std::snprintf(buf, sizeof(buf),
                  "[xp_wellys_atc][DEBUG] Recording stopped: %.1fs, %zu "
                  "samples\n",
                  last_duration_, last_samples_);
    XPLMDebugString(buf);
    std::snprintf(buf, sizeof(buf),
                  "[xp_wellys_atc][DEBUG] WAV encoded: %zu bytes\n",
                  last_wav_bytes_);
    XPLMDebugString(buf);
  }

  state_ = PTTState::PROCESSING;

  // Build airport context for Whisper prompt (improves transcription of
  // airport names like "Grenchen" that Whisper might otherwise misinterpret)
  const auto &ctx_for_whisper = xplane_context::get();
  std::string airport_ctx = ctx_for_whisper.nearest_airport_id;
  if (!ctx_for_whisper.nearest_airport_name.empty())
    airport_ctx += " " + ctx_for_whisper.nearest_airport_name;

  whisper_client::transcribe_async(
      std::move(wav),
      [](const whisper_client::TranscriptResult &wr) {
        if (!wr.success) {
          logging::error("Whisper error: %s", wr.text.c_str());
          // Show error in transcript
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
        ++total_api_calls_;

        const auto &ctx = xplane_context::get();
        float active_freq =
            (ctx.active_com == 1) ? ctx.com1_freq_mhz : ctx.com2_freq_mhz;
        char freq_str[16];
        std::snprintf(freq_str, sizeof(freq_str), "%.3f", active_freq);

        // Transcript + voice are UI concerns, done here before handing off
        // to the engine. Low-quality transcripts skip the pilot-row and go
        // straight to a "say again" response (engine produces it).
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
        std::string voice_copy = voice_for_freq(ctx.frequency_type);

        engine::Input in{
            wr.text,
            wr.quality,
            &ctx,
            settings::pilot_callsign(),
            settings::gpt_fallback_enabled(),
        };

        engine::process_transcript(
            std::move(in), [freq_str_copy, voice_copy,
                            is_pilot_row_written](const engine::Output &out) {
              last_pilot_message_ = out.parsed;
              if (out.response_text.empty()) {
                state_ = PTTState::IDLE;
                return;
              }
              // Quality-rejection path didn't write a pilot row — the ATC
              // "say again" still deserves a transcript entry with freq.
              std::string freq_for_atc =
                  is_pilot_row_written ? freq_str_copy : std::string();
              transcript_.push_back(TranscriptEntry{
                  static_cast<double>(XPLMGetElapsedTime()),
                  false,
                  out.response_text,
                  freq_for_atc,
              });
              speak_response(out.response_text, 1.0f, voice_copy);
            });
      },
      airport_ctx);
}

void update() {
  if (state_ == PTTState::PLAYING && !audio_player::is_playing()) {
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

  bool atc_idle =
      atc_state_machine::get_state() == atc_state_machine::ATCState::IDLE;
  if (state_ == PTTState::IDLE && atis_cooldown_ <= 0.0f && tuned &&
      atis_tuned_timer_ >= kAtisTuneDelaySec && atc_idle) {
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
    speak_response(atis_text, 0.85f, settings::tts_voice_atis());
  }
}

PTTState ptt_state() { return state_; }

std::string ptt_state_label() {
  switch (state_) {
  case PTTState::IDLE:
    return "Ready";
  case PTTState::RECORDING:
    return "\xE2\x97\x8F REC"; // ● REC
  case PTTState::PROCESSING:
    return "\xE2\x9F\xB3 Processing..."; // ⟳ Processing...
  case PTTState::PLAYING:
    return "\xE2\x96\xB6 ATC speaking..."; // ▶ ATC speaking...
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
int total_api_calls() { return total_api_calls_ + engine::gpt_api_calls(); }

} // namespace atc_session
