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
#include "atis_generator.hpp"
#include "atc_state_machine.hpp"
#include "atc_templates.hpp"
#include "audio_player.hpp"
#include "audio_recorder.hpp"
#include "gpt_client.hpp"
#include "intent_parser.hpp"
#include "settings.hpp"
#include "tts_client.hpp"
#include "whisper_client.hpp"
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
static float atis_tuned_timer_ = 0.0f;        // how long tuned to ATIS freq
static constexpr float kAtisTuneDelaySec = 2.0f; // wait before playing

// Speak ATC response via TTS, then transition to PLAYING → IDLE
static void speak_response(const std::string &text, float speed = 1.0f) {
  state_ = PTTState::PLAYING;
  ++total_api_calls_; // TTS call

  tts_client::speak_async(
      text,
      [](const std::vector<uint8_t> &mp3_data, bool success) {
        if (success && !mp3_data.empty()) {
          if (settings::debug_logging()) {
            char dbg[128];
            std::snprintf(dbg, sizeof(dbg),
                          "[xp_wellys_atc][DEBUG] TTS response: %zu bytes MP3\n",
                          mp3_data.size());
            XPLMDebugString(dbg);
            XPLMDebugString("[xp_wellys_atc][DEBUG] Playback started\n");
          }
          audio_player::play(mp3_data, settings::volume());
        } else {
          XPLMDebugString(
              "[xp_wellys_atc][ERROR] TTS failed, skipping playback\n");
          state_ = PTTState::IDLE;
        }
      },
      speed);
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

  // Radio requires avionics power
  const auto &ctx = xplane_context::get();
  if (!ctx.avionics_on) {
    XPLMDebugString("[xp_wellys_atc] PTT blocked — avionics off\n");
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

  whisper_client::transcribe_async(
      std::move(wav), [](const std::string &transcript, bool success) {
        if (!success) {
          XPLMDebugString(
              ("[xp_wellys_atc][ERROR] Whisper error: " + transcript + "\n")
                  .c_str());
          // Show error in transcript
          transcript_.push_back(TranscriptEntry{
              static_cast<double>(XPLMGetElapsedTime()),
              false,
              transcript,
              "",
          });
          state_ = PTTState::IDLE;
          return;
        }

        ++total_transcriptions_;
        ++total_api_calls_;

        if (settings::debug_logging())
          XPLMDebugString(("[xp_wellys_atc][DEBUG] Whisper response: \"" +
                           transcript + "\"\n")
                              .c_str());

        const auto &ctx = xplane_context::get();
        float active_freq =
            (ctx.active_com == 1) ? ctx.com1_freq_mhz : ctx.com2_freq_mhz;
        char freq_str[16];
        std::snprintf(freq_str, sizeof(freq_str), "%.3f", active_freq);

        // Add pilot message to transcript
        transcript_.push_back(TranscriptEntry{
            static_cast<double>(XPLMGetElapsedTime()),
            true,
            transcript,
            freq_str,
        });

        // Parse intent
        last_pilot_message_ = intent_parser::parse(transcript, ctx);

        if (settings::debug_logging()) {
          char log[512];
          std::snprintf(log, sizeof(log),
                        "[xp_wellys_atc][DEBUG] Intent: %s (confidence=%.2f), "
                        "callsign=%s\n",
                        intent_parser::intent_name(last_pilot_message_.intent),
                        last_pilot_message_.confidence,
                        last_pilot_message_.callsign.empty()
                            ? "(none)"
                            : last_pilot_message_.callsign.c_str());
          XPLMDebugString(log);
        }

        // Two-stage intent resolution
        bool needs_gpt =
            last_pilot_message_.confidence < 0.7f ||
            last_pilot_message_.intent == intent_parser::PilotIntent::UNKNOWN;

        if (!needs_gpt) {
          // High confidence: process through state machine directly
          auto atc_resp =
              atc_state_machine::process(last_pilot_message_, ctx);

          if (settings::debug_logging())
            XPLMDebugString(("[xp_wellys_atc][DEBUG] ATC response text: " +
                             atc_resp.text + "\n")
                                .c_str());

          transcript_.push_back(TranscriptEntry{
              static_cast<double>(XPLMGetElapsedTime()),
              false,
              atc_resp.text,
              freq_str,
          });
          speak_response(atc_resp.text);
        } else if (!settings::gpt_fallback_enabled()) {
          // GPT disabled — use state machine with _INVALID fallback
          auto atc_resp =
              atc_state_machine::process(last_pilot_message_, ctx);
          std::string response = atc_resp.text;
          if (response.empty()) {
            std::string cs = last_pilot_message_.callsign.empty()
                                 ? settings::pilot_callsign()
                                 : last_pilot_message_.callsign;
            response = "Say again, " + cs + ".";
          }

          XPLMDebugString(
              ("[xp_wellys_atc] ATC (fallback): " + response + "\n").c_str());
          transcript_.push_back(TranscriptEntry{
              static_cast<double>(XPLMGetElapsedTime()),
              false,
              response,
              freq_str,
          });
          speak_response(response);
        } else {
          // GPT intent classification
          ++total_api_calls_;

          using FT = xplane_context::FrequencyType;
          bool is_towered =
              ctx.is_towered_airport &&
              ctx.frequency_type != FT::UNICOM &&
              ctx.frequency_type != FT::CTAF;

          std::string state_str = atc_state_machine::state_name(
              atc_state_machine::get_state());
          auto valid =
              atc_templates::valid_intents(is_towered, state_str);

          std::string valid_list;
          for (const auto &v : valid) {
            if (!valid_list.empty())
              valid_list += ", ";
            valid_list += v;
          }

          std::string sys_prompt =
              atc_templates::get_prompt("gpt_classify_prompt");
          if (sys_prompt.empty()) {
            sys_prompt =
                "You are an ATC intent classifier. The pilot is in "
                "state {state}. Valid intents: {valid_intents}. The "
                "pilot said: \"{transcript}\"\nRespond with ONLY the "
                "intent name, nothing else. If none match, respond "
                "with \"_INVALID\".";
          }
          sys_prompt = atc_templates::fill(
              sys_prompt,
              {{"state", state_str},
               {"valid_intents", valid_list},
               {"transcript", transcript}});

          XPLMDebugString(
              "[xp_wellys_atc] Routing to GPT intent classification\n");

          std::string freq_copy = freq_str;
          auto msg_copy = last_pilot_message_;
          const auto &ctx_copy = ctx;

          gpt_client::classify_intent_async(
              transcript, sys_prompt,
              // NOLINTNEXTLINE(bugprone-exception-escape)
              [freq_copy, msg_copy, ctx_copy, is_towered,
               state_str](std::string intent_key, bool gpt_success) {
                if (!gpt_success)
                  intent_key = "_INVALID";

                if (settings::debug_logging()) {
                  XPLMDebugString(
                      ("[xp_wellys_atc][DEBUG] GPT classified intent: " +
                       intent_key + "\n")
                          .c_str());
                }

                auto vars =
                    atc_state_machine::build_vars(msg_copy, ctx_copy);
                auto tmpl = atc_templates::lookup(
                    is_towered, state_str, intent_key);
                std::string response =
                    atc_templates::fill(tmpl.response_template, vars);

                // Apply state transition
                auto next_state =
                    atc_state_machine::state_from_name(tmpl.next_state);
                atc_state_machine::set_state(next_state);

                transcript_.push_back(TranscriptEntry{
                    static_cast<double>(XPLMGetElapsedTime()),
                    false,
                    response,
                    freq_copy,
                });
                speak_response(response);
              });
        }
      });
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

  // ATIS playback trigger — requires avionics + tuning delay
  const auto &ctx = xplane_context::get();
  bool tuned = ctx.avionics_on && atis_generator::is_tuned_to_atis(ctx);

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
    speak_response(atis_text, 0.85f);
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
int total_api_calls() { return total_api_calls_; }

} // namespace atc_session
