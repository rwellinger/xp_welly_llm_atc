#include "audio_player.hpp"
#include "settings.hpp"

#include <XPLMUtilities.h>

#include <AudioToolbox/AudioQueue.h>
#include <CoreAudio/CoreAudio.h>

#include <cmath>
#include <cstring>

namespace audio_player {

static constexpr float kClickFreqHz = 880.0f;
static constexpr float kClickDurationSec = 0.08f;
static constexpr float kSampleRate = 44100.0f;

static std::vector<AudioDevice> devices_;

// ── Device enumeration ───────────────────────────────────────────

static std::string cf_string_to_std(CFStringRef cf) {
  if (!cf)
    return "";
  char buf[256] = {};
  if (CFStringGetCString(cf, buf, sizeof(buf), kCFStringEncodingUTF8))
    return buf;
  return "";
}

void refresh_devices() {
  devices_.clear();
  devices_.push_back({"", "System Default"});

  AudioObjectPropertyAddress prop{};
  prop.mSelector = kAudioHardwarePropertyDevices;
  prop.mScope = kAudioObjectPropertyScopeGlobal;
  prop.mElement = kAudioObjectPropertyElementMain;

  UInt32 size = 0;
  OSStatus err = AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &prop,
                                                0, nullptr, &size);
  if (err != noErr || size == 0)
    return;

  int count = static_cast<int>(size / sizeof(AudioDeviceID));
  std::vector<AudioDeviceID> ids(count);
  err = AudioObjectGetPropertyData(kAudioObjectSystemObject, &prop, 0, nullptr,
                                   &size, ids.data());
  if (err != noErr)
    return;

  for (auto dev_id : ids) {
    // Check if device has output channels
    AudioObjectPropertyAddress stream_prop{};
    stream_prop.mSelector = kAudioDevicePropertyStreamConfiguration;
    stream_prop.mScope = kAudioDevicePropertyScopeOutput;
    stream_prop.mElement = kAudioObjectPropertyElementMain;

    UInt32 stream_size = 0;
    err = AudioObjectGetPropertyDataSize(dev_id, &stream_prop, 0, nullptr,
                                         &stream_size);
    if (err != noErr || stream_size == 0)
      continue;

    std::vector<uint8_t> buf(stream_size);
    auto *list = reinterpret_cast<AudioBufferList *>(buf.data());
    err = AudioObjectGetPropertyData(dev_id, &stream_prop, 0, nullptr,
                                     &stream_size, list);
    if (err != noErr)
      continue;

    // Sum output channels
    UInt32 out_channels = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i)
      out_channels += list->mBuffers[i].mNumberChannels;
    if (out_channels == 0)
      continue; // input-only device

    // Get device UID
    AudioObjectPropertyAddress uid_prop{};
    uid_prop.mSelector = kAudioDevicePropertyDeviceUID;
    uid_prop.mScope = kAudioObjectPropertyScopeGlobal;
    uid_prop.mElement = kAudioObjectPropertyElementMain;

    CFStringRef uid_ref = nullptr;
    UInt32 uid_size = sizeof(CFStringRef);
    err = AudioObjectGetPropertyData(dev_id, &uid_prop, 0, nullptr, &uid_size,
                                     static_cast<void *>(&uid_ref));
    if (err != noErr || !uid_ref)
      continue;
    std::string uid = cf_string_to_std(uid_ref);
    CFRelease(uid_ref);

    // Get device name
    AudioObjectPropertyAddress name_prop{};
    name_prop.mSelector = kAudioObjectPropertyName;
    name_prop.mScope = kAudioObjectPropertyScopeGlobal;
    name_prop.mElement = kAudioObjectPropertyElementMain;

    CFStringRef name_ref = nullptr;
    UInt32 name_size = sizeof(CFStringRef);
    err = AudioObjectGetPropertyData(dev_id, &name_prop, 0, nullptr,
                                     &name_size,
                                     static_cast<void *>(&name_ref));
    if (err != noErr || !name_ref)
      continue;
    std::string name = cf_string_to_std(name_ref);
    CFRelease(name_ref);

    if (!uid.empty() && !name.empty())
      devices_.push_back({uid, name});
  }

  char log[128];
  std::snprintf(log, sizeof(log),
                "[xp_wellys_atc] Found %zu audio output devices\n",
                devices_.size());
  XPLMDebugString(log);
}

const std::vector<AudioDevice> &get_output_devices() { return devices_; }

// ── Audio queue callback ─────────────────────────────────────────

static void audio_queue_cb(void *, AudioQueueRef queue, AudioQueueBufferRef) {
  AudioQueueStop(queue, false);
  AudioQueueDispose(queue, false);
}

// ── Lifecycle ────────────────────────────────────────────────────

void init() { refresh_devices(); }

void stop() { devices_.clear(); }

// ── PTT click ────────────────────────────────────────────────────

void play_ptt_click() {
  float volume = settings::volume();
  if (volume <= 0.0f)
    return;

  int num_samples = static_cast<int>(kSampleRate * kClickDurationSec);

  // Generate sine wave with fade-in/out envelope
  std::vector<int16_t> samples(num_samples);
  for (int i = 0; i < num_samples; ++i) {
    float t = static_cast<float>(i) / kSampleRate;
    float sine = std::sin(2.0f * static_cast<float>(M_PI) * kClickFreqHz * t);

    float env = 1.0f;
    float fade_in = 0.005f * kSampleRate;
    float fade_out = 0.01f * kSampleRate;
    if (static_cast<float>(i) < fade_in)
      env = static_cast<float>(i) / fade_in;
    else if (static_cast<float>(i) >
             static_cast<float>(num_samples) - fade_out)
      env = (static_cast<float>(num_samples) - static_cast<float>(i)) /
            fade_out;

    samples[i] = static_cast<int16_t>(sine * env * volume * 16000.0f);
  }

  // Set up AudioQueue
  AudioStreamBasicDescription fmt{};
  fmt.mSampleRate = kSampleRate;
  fmt.mFormatID = kAudioFormatLinearPCM;
  fmt.mFormatFlags =
      kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked;
  fmt.mBitsPerChannel = 16;
  fmt.mChannelsPerFrame = 1;
  fmt.mBytesPerFrame = 2;
  fmt.mFramesPerPacket = 1;
  fmt.mBytesPerPacket = 2;

  AudioQueueRef queue = nullptr;
  OSStatus err = AudioQueueNewOutput(&fmt, audio_queue_cb, nullptr, nullptr,
                                     nullptr, 0, &queue);
  if (err != noErr || !queue) {
    XPLMDebugString(
        "[xp_wellys_atc] Failed to create audio queue for PTT click\n");
    return;
  }

  // Route to selected device
  std::string device_uid = settings::audio_output_device();
  if (!device_uid.empty()) {
    CFStringRef uid_ref = CFStringCreateWithCString(
        kCFAllocatorDefault, device_uid.c_str(), kCFStringEncodingUTF8);
    if (uid_ref) {
      AudioQueueSetProperty(queue, kAudioQueueProperty_CurrentDevice,
                            static_cast<const void *>(&uid_ref),
                            sizeof(CFStringRef));
      CFRelease(uid_ref);
    }
  }

  AudioQueueBufferRef buf = nullptr;
  UInt32 buf_size = static_cast<UInt32>(num_samples * 2);
  err = AudioQueueAllocateBuffer(queue, buf_size, &buf);
  if (err != noErr || !buf) {
    AudioQueueDispose(queue, true);
    return;
  }

  std::memcpy(buf->mAudioData, samples.data(), buf_size);
  buf->mAudioDataByteSize = buf_size;

  AudioQueueEnqueueBuffer(queue, buf, 0, nullptr);
  AudioQueueStart(queue, nullptr);
}

} // namespace audio_player
