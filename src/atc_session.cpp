#include "atc_session.hpp"
#include "atc_state_machine.hpp"
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

// Speak ATC response via TTS, then transition to PLAYING → IDLE
static void speak_response(const std::string &text) {
  state_ = PTTState::PLAYING;
  ++total_api_calls_; // TTS call

  tts_client::speak_async(text, [](const std::vector<uint8_t> &mp3_data,
                                   bool success) {
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
      // is_playing() will be polled in flight loop → when done → IDLE
    } else {
      XPLMDebugString("[xp_wellys_atc][ERROR] TTS failed, skipping playback\n");
      state_ = PTTState::IDLE;
    }
  });
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

        // Process through ATC state machine
        auto atc_resp = atc_state_machine::process(last_pilot_message_, ctx);

        bool needs_gpt =
            atc_resp.text.empty() || last_pilot_message_.confidence < 0.6f ||
            last_pilot_message_.intent == intent_parser::PilotIntent::UNKNOWN;

        if (needs_gpt) {
          if (!settings::gpt_fallback_enabled()) {
            // Generic fallback
            std::string cs = last_pilot_message_.callsign.empty()
                                 ? settings::pilot_callsign()
                                 : last_pilot_message_.callsign;
            std::string fallback = "Say again, " + cs + ".";
            XPLMDebugString(
                ("[xp_wellys_atc] ATC (fallback): " + fallback + "\n").c_str());

            transcript_.push_back(TranscriptEntry{
                static_cast<double>(XPLMGetElapsedTime()),
                false,
                fallback,
                freq_str,
            });
            speak_response(fallback);
          } else {
            // GPT fallback — stays in PROCESSING until callback
            ++total_api_calls_; // GPT call
            XPLMDebugString("[xp_wellys_atc] Routing to GPT fallback\n");

            std::string freq_copy = freq_str;
            gpt_client::ask_async(
                transcript, ctx,
                [freq_copy](std::string response, bool gpt_success) {
                  if (gpt_success) {
                    XPLMDebugString(
                        ("[xp_wellys_atc] GPT response: " + response + "\n")
                            .c_str());
                  } else {
                    XPLMDebugString(
                        ("[xp_wellys_atc] GPT error: " + response + "\n")
                            .c_str());
                    std::string cs = settings::pilot_callsign();
                    response = "Say again, " + cs + ".";
                  }

                  transcript_.push_back(TranscriptEntry{
                      static_cast<double>(XPLMGetElapsedTime()),
                      false,
                      response,
                      freq_copy,
                  });
                  speak_response(response);
                });
          }
        } else {
          // Valid state machine response
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
        }
      });
}

void update() {
  if (state_ == PTTState::PLAYING && !audio_player::is_playing()) {
    if (settings::debug_logging())
      XPLMDebugString(
          "[xp_wellys_atc][DEBUG] Playback finished, state -> IDLE\n");
    state_ = PTTState::IDLE;
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

int total_transcriptions() { return total_transcriptions_; }
int total_api_calls() { return total_api_calls_; }

} // namespace atc_session
