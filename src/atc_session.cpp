#include "atc_session.hpp"
#include "audio_recorder.hpp"

#include <XPLMUtilities.h>

#include <cstdio>
#include <string>

namespace atc_session {

static PTTState state_ = PTTState::IDLE;

static float last_duration_ = 0.0f;
static size_t last_samples_ = 0;
static size_t last_wav_bytes_ = 0;

void init() {
  state_ = PTTState::IDLE;
  last_duration_ = 0.0f;
  last_samples_ = 0;
  last_wav_bytes_ = 0;
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

  state_ = PTTState::RECORDING;
  audio_recorder::start_recording();
  XPLMDebugString("[xp_wellys_atc] PTT pressed — recording started\n");
}

void on_ptt_released() {
  if (state_ != PTTState::RECORDING)
    return;

  audio_recorder::stop_recording();

  last_duration_ = audio_recorder::duration_seconds();
  last_samples_ = audio_recorder::buffer_samples();

  auto wav = audio_recorder::encode_wav();
  last_wav_bytes_ = wav.size();

  char buf[256];
  std::snprintf(buf, sizeof(buf),
                "[xp_wellys_atc] PTT released — duration=%.2fs, samples=%zu, "
                "wav=%zu bytes\n",
                last_duration_, last_samples_, last_wav_bytes_);
  XPLMDebugString(buf);

  // Transition back to IDLE (Whisper not yet implemented — will change in M3)
  state_ = PTTState::IDLE;
}

PTTState ptt_state() { return state_; }

std::string ptt_state_label() {
  switch (state_) {
  case PTTState::IDLE:
    return "IDLE";
  case PTTState::RECORDING:
    return "\xE2\x97\x8F REC"; // ● REC
  case PTTState::PROCESSING:
    return "\xE2\x9F\xB3 PROCESSING"; // ⟳ PROCESSING
  case PTTState::PLAYING:
    return "\xE2\x96\xB6 PLAYING"; // ▶ PLAYING
  }
  return "UNKNOWN";
}

float last_recording_duration() { return last_duration_; }
size_t last_recording_samples() { return last_samples_; }
size_t last_wav_bytes() { return last_wav_bytes_; }

} // namespace atc_session
