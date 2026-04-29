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

#include "audio/audio_recorder.hpp"
#include "audio/mic_permission.hpp"
#include "persistence/settings.hpp"

#include <XPLMUtilities.h>

#include <atomic>
#include <cstring>
#include <mutex>
#include <vector>

#if defined(__APPLE__)
#include <AudioToolbox/AudioToolbox.h>
#include <AudioUnit/AudioUnit.h>
#endif

namespace audio_recorder {

static constexpr unsigned kDesiredSampleRate = 16000;
static constexpr unsigned kBitsPerSample = 16;
static constexpr unsigned kNumChannels = 1;

static unsigned actual_sample_rate_ = kDesiredSampleRate;

static std::vector<int16_t> buffer_;
static std::mutex buffer_mutex_;
static std::atomic<bool> recording_{false};
static bool initialized_ = false;

#if defined(__APPLE__)

static AudioComponentInstance audio_unit_ = nullptr;

static std::atomic<int> render_call_count_{0};

static OSStatus render_callback(void * /*inRefCon*/,
                                AudioUnitRenderActionFlags *io_action_flags,
                                const AudioTimeStamp *in_time_stamp,
                                UInt32 in_bus_number, UInt32 in_number_frames,
                                AudioBufferList * /*io_data*/) {
  int call_count = ++render_call_count_;
  if (call_count == 1) {
    if (settings::debug_logging())
      XPLMDebugString(
          "[xp_wellys_atc][DEBUG] Render callback firing (first call)\n");
  }

  AudioBufferList buf_list;
  buf_list.mNumberBuffers = 1;
  buf_list.mBuffers[0].mDataByteSize = in_number_frames * sizeof(int16_t);
  buf_list.mBuffers[0].mNumberChannels = 1;

  std::vector<int16_t> temp(in_number_frames);
  buf_list.mBuffers[0].mData = temp.data();

  OSStatus status = AudioUnitRender(audio_unit_, io_action_flags, in_time_stamp,
                                    1, in_number_frames, &buf_list);
  if (status != noErr) {
    if (call_count <= 3) {
      char log[128];
      std::snprintf(log, sizeof(log),
                    "[xp_wellys_atc] AudioUnitRender error: %d (call #%d)\n",
                    static_cast<int>(status), call_count);
      XPLMDebugString(log);
    }
    return status;
  }

  if (recording_.load()) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    buffer_.insert(buffer_.end(), temp.begin(), temp.end());
  }

  return noErr;
}

static void log_default_input_device() {
  AudioObjectPropertyAddress prop{};
  prop.mSelector = kAudioHardwarePropertyDefaultInputDevice;
  prop.mScope = kAudioObjectPropertyScopeGlobal;
  prop.mElement = kAudioObjectPropertyElementMain;

  AudioDeviceID dev_id = kAudioDeviceUnknown;
  UInt32 size = sizeof(dev_id);
  OSStatus err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &prop, 0,
                                            nullptr, &size, &dev_id);
  if (err != noErr || dev_id == kAudioDeviceUnknown) {
    XPLMDebugString("[xp_wellys_atc] No default input device found\n");
    return;
  }

  AudioObjectPropertyAddress name_prop{};
  name_prop.mSelector = kAudioObjectPropertyName;
  name_prop.mScope = kAudioObjectPropertyScopeGlobal;
  name_prop.mElement = kAudioObjectPropertyElementMain;
  CFStringRef name_ref = nullptr;
  UInt32 name_size = sizeof(CFStringRef);
  err = AudioObjectGetPropertyData(dev_id, &name_prop, 0, nullptr, &name_size,
                                   static_cast<void *>(&name_ref));
  if (err == noErr && name_ref) {
    char name[256] = {};
    CFStringGetCString(name_ref, name, sizeof(name), kCFStringEncodingUTF8);
    CFRelease(name_ref);
    char log[320];
    std::snprintf(log, sizeof(log),
                  "[xp_wellys_atc] Default input device: \"%s\" (id=%u)\n",
                  name, static_cast<unsigned>(dev_id));
    XPLMDebugString(log);
  }
}

void init() {
  if (!mic_permission::check_and_request()) {
    XPLMDebugString("[xp_wellys_atc] Audio recorder: no microphone permission, "
                    "recording will not work\n");
  }

  log_default_input_device();

  AudioComponentDescription desc{};
  desc.componentType = kAudioUnitType_Output;
  desc.componentSubType = kAudioUnitSubType_HALOutput;
  desc.componentManufacturer = kAudioUnitManufacturer_Apple;

  AudioComponent component = AudioComponentFindNext(nullptr, &desc);
  if (!component) {
    XPLMDebugString(
        "[xp_wellys_atc] Error: no HALOutput audio component found\n");
    return;
  }

  OSStatus status = AudioComponentInstanceNew(component, &audio_unit_);
  if (status != noErr) {
    XPLMDebugString("[xp_wellys_atc] Error: failed to create AudioUnit\n");
    return;
  }

  // Enable input (bus 1)
  UInt32 enable_input = 1;
  status = AudioUnitSetProperty(audio_unit_, kAudioOutputUnitProperty_EnableIO,
                                kAudioUnitScope_Input, 1, &enable_input,
                                sizeof(enable_input));
  if (status != noErr) {
    XPLMDebugString("[xp_wellys_atc] Error: failed to enable audio input\n");
    AudioComponentInstanceDispose(audio_unit_);
    audio_unit_ = nullptr;
    return;
  }

  // Disable output (bus 0)
  UInt32 disable_output = 0;
  AudioUnitSetProperty(audio_unit_, kAudioOutputUnitProperty_EnableIO,
                       kAudioUnitScope_Output, 0, &disable_output,
                       sizeof(disable_output));

  // Explicitly set input device to system default input
  AudioObjectPropertyAddress default_input_prop{};
  default_input_prop.mSelector = kAudioHardwarePropertyDefaultInputDevice;
  default_input_prop.mScope = kAudioObjectPropertyScopeGlobal;
  default_input_prop.mElement = kAudioObjectPropertyElementMain;
  AudioDeviceID input_device = kAudioDeviceUnknown;
  UInt32 dev_size = sizeof(input_device);
  AudioObjectGetPropertyData(kAudioObjectSystemObject, &default_input_prop, 0,
                             nullptr, &dev_size, &input_device);
  if (input_device != kAudioDeviceUnknown) {
    status = AudioUnitSetProperty(
        audio_unit_, kAudioOutputUnitProperty_CurrentDevice,
        kAudioUnitScope_Global, 0, &input_device, sizeof(input_device));
    char log[128];
    std::snprintf(log, sizeof(log),
                  "[xp_wellys_atc] Set AudioUnit input device id=%u: %s\n",
                  static_cast<unsigned>(input_device),
                  status == noErr ? "OK" : "FAILED");
    XPLMDebugString(log);
  }

  // Log the device's native (hardware) format
  AudioStreamBasicDescription hw_fmt{};
  UInt32 hw_fmt_size = sizeof(hw_fmt);
  status =
      AudioUnitGetProperty(audio_unit_, kAudioUnitProperty_StreamFormat,
                           kAudioUnitScope_Input, 1, &hw_fmt, &hw_fmt_size);
  if (status == noErr) {
    char log[256];
    std::snprintf(log, sizeof(log),
                  "[xp_wellys_atc] Hardware input format: %.0f Hz, %u ch, "
                  "%u bps, formatFlags=0x%X\n",
                  hw_fmt.mSampleRate,
                  static_cast<unsigned>(hw_fmt.mChannelsPerFrame),
                  static_cast<unsigned>(hw_fmt.mBitsPerChannel),
                  static_cast<unsigned>(hw_fmt.mFormatFlags));
    XPLMDebugString(log);
  }

  // Use the device's native sample rate to avoid AudioUnit conversion errors
  // (e.g. -10863 when converting 24kHz AirPods → 16kHz).
  // Only convert to int16 PCM; Whisper accepts any sample rate.
  double device_rate =
      (hw_fmt.mSampleRate > 0) ? hw_fmt.mSampleRate : kDesiredSampleRate;

  AudioStreamBasicDescription format{};
  format.mSampleRate = device_rate;
  format.mFormatID = kAudioFormatLinearPCM;
  format.mFormatFlags =
      kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
  format.mBytesPerPacket = sizeof(int16_t);
  format.mFramesPerPacket = 1;
  format.mBytesPerFrame = sizeof(int16_t);
  format.mChannelsPerFrame = kNumChannels;
  format.mBitsPerChannel = kBitsPerSample;

  status =
      AudioUnitSetProperty(audio_unit_, kAudioUnitProperty_StreamFormat,
                           kAudioUnitScope_Output, 1, &format, sizeof(format));
  if (status != noErr) {
    XPLMDebugString("[xp_wellys_atc] Error: failed to set audio format\n");
    AudioComponentInstanceDispose(audio_unit_);
    audio_unit_ = nullptr;
    return;
  }

  // Read back actual format — HALOutput may not do sample rate conversion
  AudioStreamBasicDescription actual_fmt{};
  UInt32 fmt_size = sizeof(actual_fmt);
  status =
      AudioUnitGetProperty(audio_unit_, kAudioUnitProperty_StreamFormat,
                           kAudioUnitScope_Output, 1, &actual_fmt, &fmt_size);
  if (status == noErr) {
    actual_sample_rate_ = static_cast<unsigned>(actual_fmt.mSampleRate);
    char log[192];
    std::snprintf(log, sizeof(log),
                  "[xp_wellys_atc] Audio format: device %.0f Hz, client %u Hz "
                  "(%u ch, %u bps)\n",
                  device_rate, actual_sample_rate_,
                  static_cast<unsigned>(actual_fmt.mChannelsPerFrame),
                  static_cast<unsigned>(actual_fmt.mBitsPerChannel));
    XPLMDebugString(log);
  }

  // Set render callback on input bus
  AURenderCallbackStruct callback{};
  callback.inputProc = render_callback;
  callback.inputProcRefCon = nullptr;

  status = AudioUnitSetProperty(
      audio_unit_, kAudioOutputUnitProperty_SetInputCallback,
      kAudioUnitScope_Global, 0, &callback, sizeof(callback));
  if (status != noErr) {
    XPLMDebugString("[xp_wellys_atc] Error: failed to set render callback\n");
    AudioComponentInstanceDispose(audio_unit_);
    audio_unit_ = nullptr;
    return;
  }

  status = AudioUnitInitialize(audio_unit_);
  if (status != noErr) {
    XPLMDebugString("[xp_wellys_atc] Error: failed to initialize AudioUnit\n");
    AudioComponentInstanceDispose(audio_unit_);
    audio_unit_ = nullptr;
    return;
  }

  initialized_ = true;
  {
    char log[128];
    std::snprintf(
        log, sizeof(log),
        "[xp_wellys_atc] Audio recorder initialized (%uHz mono 16-bit)\n",
        actual_sample_rate_);
    XPLMDebugString(log);
  }
}

void stop() {
  if (audio_unit_) {
    AudioOutputUnitStop(audio_unit_);
    AudioUnitUninitialize(audio_unit_);
    AudioComponentInstanceDispose(audio_unit_);
    audio_unit_ = nullptr;
  }
  recording_ = false;
  initialized_ = false;
  std::lock_guard<std::mutex> lock(buffer_mutex_);
  buffer_.clear();
}

void start_recording() {
  if (!initialized_ || !audio_unit_) {
    XPLMDebugString(
        "[xp_wellys_atc] Warning: audio recorder not initialized\n");
    return;
  }

  {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    buffer_.clear();
  }

  render_call_count_ = 0;
  recording_ = true;
  OSStatus status = AudioOutputUnitStart(audio_unit_);
  if (status != noErr) {
    char log[128];
    std::snprintf(log, sizeof(log),
                  "[xp_wellys_atc] Error: AudioOutputUnitStart failed: %d\n",
                  static_cast<int>(status));
    XPLMDebugString(log);
    recording_ = false;
  } else {
    if (settings::debug_logging())
      XPLMDebugString("[xp_wellys_atc][DEBUG] AudioUnit started OK\n");
  }
}

void stop_recording() {
  recording_ = false;
  if (audio_unit_) {
    AudioOutputUnitStop(audio_unit_);
  }
  {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    // Compute peak level
    int16_t peak = 0;
    for (auto s : buffer_) {
      int16_t abs_s = (s < 0) ? static_cast<int16_t>(-s) : s;
      if (abs_s > peak)
        peak = abs_s;
    }
    float peak_pct = (static_cast<float>(peak) / 32767.0f) * 100.0f;

    if (settings::debug_logging()) {
      char log[256];
      std::snprintf(
          log, sizeof(log),
          "[xp_wellys_atc][DEBUG] Recording stopped: %zu samples captured, "
          "render callbacks: %d, peak: %d (%.1f%%)\n",
          buffer_.size(), render_call_count_.load(), static_cast<int>(peak),
          peak_pct);
      XPLMDebugString(log);
    }
    if (buffer_.empty() && render_call_count_.load() == 0) {
      XPLMDebugString(
          "[xp_wellys_atc] ERROR: No audio captured. Check: System Settings "
          "> Privacy & Security > Microphone > enable X-Plane\n");
    }
  }
}

#else

void init() {
  XPLMDebugString("[xp_wellys_atc] Warning: audio recorder not supported on "
                  "this platform\n");
}
void stop() {}
void start_recording() {}
void stop_recording() {}

#endif

std::vector<uint8_t> encode_wav() {
  std::lock_guard<std::mutex> lock(buffer_mutex_);

  uint32_t data_size = static_cast<uint32_t>(buffer_.size() * sizeof(int16_t));
  uint32_t file_size = 36 + data_size;

  std::vector<uint8_t> wav;
  wav.reserve(44 + data_size);

  auto write_u32 = [&](uint32_t v) {
    wav.push_back(static_cast<uint8_t>(v & 0xFF));
    wav.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    wav.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    wav.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
  };
  auto write_u16 = [&](uint16_t v) {
    wav.push_back(static_cast<uint8_t>(v & 0xFF));
    wav.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
  };
  auto write_str = [&](const char *s) {
    while (*s)
      wav.push_back(static_cast<uint8_t>(*s++));
  };

  // RIFF header
  write_str("RIFF");
  write_u32(file_size);
  write_str("WAVE");

  // fmt subchunk
  write_str("fmt ");
  write_u32(16);                  // subchunk size
  write_u16(1);                   // PCM format
  write_u16(kNumChannels);        // channels
  write_u32(actual_sample_rate_); // sample rate
  write_u32(size_t{actual_sample_rate_} * kNumChannels *
            sizeof(int16_t));                        // byte rate
  write_u16(size_t{kNumChannels} * sizeof(int16_t)); // block align
  write_u16(kBitsPerSample);                         // bits per sample

  // data subchunk
  write_str("data");
  write_u32(data_size);

  // Raw PCM samples
  const auto *raw = reinterpret_cast<const uint8_t *>(buffer_.data());
  wav.insert(wav.end(), raw, raw + data_size);

  return wav;
}

float duration_seconds() {
  std::lock_guard<std::mutex> lock(buffer_mutex_);
  if (buffer_.empty())
    return 0.0f;
  return static_cast<float>(buffer_.size()) /
         static_cast<float>(actual_sample_rate_);
}

size_t buffer_samples() {
  std::lock_guard<std::mutex> lock(buffer_mutex_);
  return buffer_.size();
}

} // namespace audio_recorder
