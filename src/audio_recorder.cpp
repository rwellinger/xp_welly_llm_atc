#include "audio_recorder.hpp"

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

static constexpr unsigned kSampleRate = 16000;
static constexpr unsigned kBitsPerSample = 16;
static constexpr unsigned kNumChannels = 1;

static std::vector<int16_t> buffer_;
static std::mutex buffer_mutex_;
static std::atomic<bool> recording_{false};
static bool initialized_ = false;

#if defined(__APPLE__)

static AudioComponentInstance audio_unit_ = nullptr;

static OSStatus render_callback(void * /*inRefCon*/,
                                AudioUnitRenderActionFlags *io_action_flags,
                                const AudioTimeStamp *in_time_stamp,
                                UInt32 in_bus_number, UInt32 in_number_frames,
                                AudioBufferList * /*io_data*/) {
  AudioBufferList buf_list;
  buf_list.mNumberBuffers = 1;
  buf_list.mBuffers[0].mDataByteSize = in_number_frames * sizeof(int16_t);
  buf_list.mBuffers[0].mNumberChannels = 1;

  std::vector<int16_t> temp(in_number_frames);
  buf_list.mBuffers[0].mData = temp.data();

  OSStatus status = AudioUnitRender(audio_unit_, io_action_flags, in_time_stamp,
                                    in_bus_number, in_number_frames, &buf_list);
  if (status != noErr)
    return status;

  if (recording_.load()) {
    std::lock_guard<std::mutex> lock(buffer_mutex_);
    buffer_.insert(buffer_.end(), temp.begin(), temp.end());
  }

  return noErr;
}

void init() {
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

  // Set input format: 16kHz mono 16-bit signed integer PCM
  AudioStreamBasicDescription format{};
  format.mSampleRate = kSampleRate;
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
  XPLMDebugString(
      "[xp_wellys_atc] Audio recorder initialized (16kHz mono 16-bit)\n");
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

  recording_ = true;
  OSStatus status = AudioOutputUnitStart(audio_unit_);
  if (status != noErr) {
    XPLMDebugString("[xp_wellys_atc] Error: failed to start AudioUnit\n");
    recording_ = false;
  }
}

void stop_recording() {
  recording_ = false;
  if (audio_unit_) {
    AudioOutputUnitStop(audio_unit_);
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
  write_u32(16);                                           // subchunk size
  write_u16(1);                                            // PCM format
  write_u16(kNumChannels);                                 // channels
  write_u32(kSampleRate);                                  // sample rate
  write_u32(size_t{kSampleRate} * kNumChannels * sizeof(int16_t)); // byte rate
  write_u16(size_t{kNumChannels} * sizeof(int16_t));              // block align
  write_u16(kBitsPerSample);                               // bits per sample

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
  return static_cast<float>(buffer_.size()) / static_cast<float>(kSampleRate);
}

size_t buffer_samples() {
  std::lock_guard<std::mutex> lock(buffer_mutex_);
  return buffer_.size();
}

} // namespace audio_recorder
