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

#include "audio/audio_player.hpp"
#include "settings.hpp"

#include <XPLMSound.h>
#include <XPLMUtilities.h>

#include <AudioToolbox/AudioToolbox.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

namespace audio_player {

static constexpr float kClickFreqHz = 880.0f;
static constexpr float kClickDurationSec = 0.08f;
static constexpr float kClickSampleRate = 44100.0f;

// ── FMOD channel tracking ────────────────────────────────────────

static std::atomic<bool> is_playing_{false};
static FMOD_CHANNEL *active_channel_ = nullptr;
static std::mutex channel_mutex_;

// PCM buffer must remain valid until FMOD completion callback fires
static std::vector<int16_t> active_pcm16_;

static void pcm_complete_cb(void * /*inRefcon*/, FMOD_RESULT /*status*/) {
  std::lock_guard<std::mutex> lock(channel_mutex_);
  active_channel_ = nullptr;
  is_playing_ = false;
}

// ── Core helper: play int16 PCM on given bus ─────────────────────
// Must be called on the X-Plane main thread.

static void play_pcm16(std::vector<int16_t> pcm16, int freq_hz, int channels,
                       float volume, XPLMAudioBus bus) {
  // Stop any current playback
  {
    std::lock_guard<std::mutex> lock(channel_mutex_);
    if (active_channel_) {
      XPLMStopAudio(active_channel_);
      active_channel_ = nullptr;
    }
  }
  is_playing_ = false;

  if (pcm16.empty()) {
    XPLMDebugString("[xp_wellys_atc] play_pcm16: empty buffer\n");
    return;
  }

  // Keep PCM alive for FMOD
  active_pcm16_ = std::move(pcm16);

  FMOD_CHANNEL *ch = XPLMPlayPCMOnBus(
      active_pcm16_.data(),
      static_cast<uint32_t>(active_pcm16_.size() * sizeof(int16_t)),
      FMOD_SOUND_FORMAT_PCM16, freq_hz, channels,
      /*loop=*/0, bus, pcm_complete_cb, nullptr);

  if (!ch) {
    XPLMDebugString("[xp_wellys_atc] XPLMPlayPCMOnBus failed\n");
    return;
  }

  XPLMSetAudioVolume(ch, volume);

  {
    std::lock_guard<std::mutex> lock(channel_mutex_);
    active_channel_ = ch;
  }
  is_playing_ = true;

  const char *bus_name = "unknown";
  if (bus == xplm_AudioRadioCom1)
    bus_name = "COM1";
  else if (bus == xplm_AudioRadioCom2)
    bus_name = "COM2";
  else if (bus == xplm_AudioUI)
    bus_name = "UI";

  if (settings::debug_logging()) {
    char log[128];
    std::snprintf(
        log, sizeof(log),
        "[xp_wellys_atc][DEBUG] Playback started: %zu samples, %d Hz, "
        "%s bus\n",
        active_pcm16_.size() / channels, freq_hz, bus_name);
    XPLMDebugString(log);
  }
}

// ── MP3 decode via AudioToolbox ──────────────────────────────────

struct AudioFileReadContext {
  const uint8_t *data;
  size_t size;
};

static OSStatus audio_file_read_proc(void *inClientData, SInt64 inPosition,
                                     UInt32 requestCount, void *buffer,
                                     UInt32 *actualCount) {
  auto *ctx = static_cast<AudioFileReadContext *>(inClientData);
  if (inPosition >= static_cast<SInt64>(ctx->size)) {
    *actualCount = 0;
    return noErr;
  }
  size_t avail = ctx->size - static_cast<size_t>(inPosition);
  size_t to_read = (requestCount < avail) ? requestCount : avail;
  std::memcpy(buffer, ctx->data + inPosition, to_read);
  *actualCount = static_cast<UInt32>(to_read);
  return noErr;
}

static SInt64 audio_file_get_size_proc(void *inClientData) {
  auto *ctx = static_cast<AudioFileReadContext *>(inClientData);
  return static_cast<SInt64>(ctx->size);
}

static bool decode_mp3_to_pcm16(const std::vector<uint8_t> &mp3_data,
                                std::vector<int16_t> &out_pcm,
                                int &out_channels, int &out_sample_rate) {
  AudioFileReadContext ctx{mp3_data.data(), mp3_data.size()};

  AudioFileID audio_file = nullptr;
  OSStatus err = AudioFileOpenWithCallbacks(&ctx, audio_file_read_proc, nullptr,
                                            audio_file_get_size_proc, nullptr,
                                            kAudioFileMP3Type, &audio_file);
  if (err != noErr) {
    char log[128];
    std::snprintf(log, sizeof(log),
                  "[xp_wellys_atc] MP3 AudioFileOpen failed: %d\n",
                  static_cast<int>(err));
    XPLMDebugString(log);
    return false;
  }

  AudioStreamBasicDescription src_fmt{};
  UInt32 fmt_size = sizeof(src_fmt);
  err = AudioFileGetProperty(audio_file, kAudioFilePropertyDataFormat,
                             &fmt_size, &src_fmt);
  if (err != noErr) {
    AudioFileClose(audio_file);
    return false;
  }

  ExtAudioFileRef ext_file = nullptr;
  err = ExtAudioFileWrapAudioFileID(audio_file, false, &ext_file);
  if (err != noErr) {
    AudioFileClose(audio_file);
    return false;
  }

  // Decode to int16 PCM (native FMOD_SOUND_FORMAT_PCM16)
  AudioStreamBasicDescription dst_fmt{};
  dst_fmt.mSampleRate = src_fmt.mSampleRate;
  dst_fmt.mFormatID = kAudioFormatLinearPCM;
  dst_fmt.mFormatFlags =
      kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
  dst_fmt.mBitsPerChannel = 16;
  dst_fmt.mChannelsPerFrame = src_fmt.mChannelsPerFrame;
  dst_fmt.mBytesPerFrame = sizeof(int16_t) * dst_fmt.mChannelsPerFrame;
  dst_fmt.mFramesPerPacket = 1;
  dst_fmt.mBytesPerPacket = dst_fmt.mBytesPerFrame;

  err =
      ExtAudioFileSetProperty(ext_file, kExtAudioFileProperty_ClientDataFormat,
                              sizeof(dst_fmt), &dst_fmt);
  if (err != noErr) {
    ExtAudioFileDispose(ext_file);
    AudioFileClose(audio_file);
    return false;
  }

  SInt64 total_frames = 0;
  UInt32 prop_size = sizeof(total_frames);
  ExtAudioFileGetProperty(ext_file, kExtAudioFileProperty_FileLengthFrames,
                          &prop_size, &total_frames);

  if (total_frames <= 0) {
    ExtAudioFileDispose(ext_file);
    AudioFileClose(audio_file);
    return false;
  }

  out_pcm.resize(static_cast<size_t>(total_frames) * src_fmt.mChannelsPerFrame);

  UInt32 frames_to_read = static_cast<UInt32>(total_frames);
  AudioBufferList buf_list{};
  buf_list.mNumberBuffers = 1;
  buf_list.mBuffers[0].mNumberChannels = dst_fmt.mChannelsPerFrame;
  buf_list.mBuffers[0].mDataByteSize =
      static_cast<UInt32>(out_pcm.size() * sizeof(int16_t));
  buf_list.mBuffers[0].mData = out_pcm.data();

  err = ExtAudioFileRead(ext_file, &frames_to_read, &buf_list);
  ExtAudioFileDispose(ext_file);
  AudioFileClose(audio_file);

  if (err != noErr) {
    char log[128];
    std::snprintf(log, sizeof(log),
                  "[xp_wellys_atc] ExtAudioFileRead failed: %d\n",
                  static_cast<int>(err));
    XPLMDebugString(log);
    return false;
  }

  out_pcm.resize(static_cast<size_t>(frames_to_read) *
                 src_fmt.mChannelsPerFrame);
  out_channels = static_cast<int>(src_fmt.mChannelsPerFrame);
  out_sample_rate = static_cast<int>(src_fmt.mSampleRate);

  if (settings::debug_logging()) {
    char log[128];
    std::snprintf(
        log, sizeof(log),
        "[xp_wellys_atc][DEBUG] MP3 decoded: %u frames, %d ch, %d Hz\n",
        frames_to_read, out_channels, out_sample_rate);
    XPLMDebugString(log);
  }
  return true;
}

// ── WAV decode (PCM16 only) ───────────────────────────────────────

static bool decode_wav_to_pcm16(const std::vector<uint8_t> &wav_data,
                                std::vector<int16_t> &out_pcm,
                                int &out_channels, int &out_sample_rate) {
  if (wav_data.size() < 44)
    return false;

  if (wav_data[0] != 'R' || wav_data[1] != 'I' || wav_data[2] != 'F' ||
      wav_data[3] != 'F' || wav_data[8] != 'W' || wav_data[9] != 'A' ||
      wav_data[10] != 'V' || wav_data[11] != 'E') {
    XPLMDebugString("[xp_wellys_atc] WAV: bad magic\n");
    return false;
  }

  auto read_u16 = [&](size_t off) -> uint16_t {
    return static_cast<uint16_t>(wav_data[off]) |
           (static_cast<uint16_t>(wav_data[off + 1]) << 8);
  };
  auto read_u32 = [&](size_t off) -> uint32_t {
    return static_cast<uint32_t>(wav_data[off]) |
           (static_cast<uint32_t>(wav_data[off + 1]) << 8) |
           (static_cast<uint32_t>(wav_data[off + 2]) << 16) |
           (static_cast<uint32_t>(wav_data[off + 3]) << 24);
  };

  size_t pos = 12;
  uint16_t channels = 0;
  uint32_t sample_rate = 0;
  uint16_t bits_per_sample = 0;
  size_t data_offset = 0;
  uint32_t data_size = 0;

  while (pos + 8 <= wav_data.size()) {
    char id[5] = {};
    std::memcpy(id, wav_data.data() + pos, 4);
    uint32_t chunk_size = read_u32(pos + 4);
    pos += 8;
    if (std::strcmp(id, "fmt ") == 0 && chunk_size >= 16) {
      if (read_u16(pos) != 1) {
        XPLMDebugString("[xp_wellys_atc] WAV: not PCM\n");
        return false;
      }
      channels = read_u16(pos + 2);
      sample_rate = read_u32(pos + 4);
      bits_per_sample = read_u16(pos + 14);
    } else if (std::strcmp(id, "data") == 0) {
      data_offset = pos;
      data_size = chunk_size;
      break;
    }
    pos += chunk_size;
  }

  if (data_offset == 0 || channels == 0 || bits_per_sample != 16) {
    char log[128];
    std::snprintf(log, sizeof(log),
                  "[xp_wellys_atc] WAV: invalid (ch=%u sr=%u bps=%u)\n",
                  channels, sample_rate, bits_per_sample);
    XPLMDebugString(log);
    return false;
  }

  size_t num_samples = data_size / sizeof(int16_t);
  out_pcm.resize(num_samples);
  std::memcpy(out_pcm.data(), wav_data.data() + data_offset,
              num_samples * sizeof(int16_t));

  out_channels = static_cast<int>(channels);
  out_sample_rate = static_cast<int>(sample_rate);

  char log[128];
  std::snprintf(log, sizeof(log),
                "[xp_wellys_atc] WAV decoded: %zu frames, %d ch, %d Hz\n",
                num_samples / channels, out_channels, out_sample_rate);
  XPLMDebugString(log);
  return true;
}

// ── Lifecycle ────────────────────────────────────────────────────

void init() {
  is_playing_ = false;
  active_channel_ = nullptr;
  XPLMDebugString(
      "[xp_wellys_atc] Audio player initialized (FMOD radio bus)\n");
}

void stop() {
  std::lock_guard<std::mutex> lock(channel_mutex_);
  if (active_channel_) {
    XPLMStopAudio(active_channel_);
    active_channel_ = nullptr;
  }
  is_playing_ = false;
  active_pcm16_.clear();
}

// ── PTT click ────────────────────────────────────────────────────

void play_ptt_click() {
  float volume = settings::volume();
  if (volume <= 0.0f)
    return;

  int num_samples = static_cast<int>(kClickSampleRate * kClickDurationSec);
  std::vector<int16_t> samples(num_samples);
  for (int i = 0; i < num_samples; ++i) {
    float t = static_cast<float>(i) / kClickSampleRate;
    float sine = std::sin(2.0f * static_cast<float>(M_PI) * kClickFreqHz * t);
    float env = 1.0f;
    float fade_in = 0.005f * kClickSampleRate;
    float fade_out = 0.01f * kClickSampleRate;
    if (static_cast<float>(i) < fade_in)
      env = static_cast<float>(i) / fade_in;
    else if (static_cast<float>(i) > static_cast<float>(num_samples) - fade_out)
      env =
          (static_cast<float>(num_samples) - static_cast<float>(i)) / fade_out;
    samples[i] = static_cast<int16_t>(sine * env * 32767.0f);
  }

  XPLMAudioBus bus =
      (settings::active_com() == 2) ? xplm_AudioRadioCom2 : xplm_AudioRadioCom1;
  play_pcm16(std::move(samples), static_cast<int>(kClickSampleRate), 1, volume,
             bus);
}

// ── MP3 playback (ATC responses → radio bus) ────────────────────

void play(const std::vector<uint8_t> &mp3_data, float volume) {
  if (mp3_data.empty()) {
    XPLMDebugString("[xp_wellys_atc] play() called with empty MP3\n");
    return;
  }

  std::vector<int16_t> pcm16;
  int channels = 0;
  int sample_rate = 0;
  if (!decode_mp3_to_pcm16(mp3_data, pcm16, channels, sample_rate)) {
    XPLMDebugString("[xp_wellys_atc] MP3 decode failed\n");
    return;
  }

  XPLMAudioBus bus =
      (settings::active_com() == 2) ? xplm_AudioRadioCom2 : xplm_AudioRadioCom1;
  play_pcm16(std::move(pcm16), sample_rate, channels, volume, bus);
}

// ── WAV playback (test → UI bus, always audible) ────────────────

void play_wav(const std::vector<uint8_t> &wav_data, float volume) {
  if (wav_data.empty()) {
    XPLMDebugString("[xp_wellys_atc] play_wav() called with empty data\n");
    return;
  }

  std::vector<int16_t> pcm16;
  int channels = 0;
  int sample_rate = 0;
  if (!decode_wav_to_pcm16(wav_data, pcm16, channels, sample_rate)) {
    XPLMDebugString("[xp_wellys_atc] WAV decode failed\n");
    return;
  }

  XPLMAudioBus bus =
      (settings::active_com() == 2) ? xplm_AudioRadioCom2 : xplm_AudioRadioCom1;
  play_pcm16(std::move(pcm16), sample_rate, channels, volume, bus);
}

bool is_playing() { return is_playing_.load(); }

} // namespace audio_player
