#include "audio_player.hpp"
#include "settings.hpp"

#include <XPLMUtilities.h>

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>

namespace audio_player {

static constexpr float kClickFreqHz = 880.0f;
static constexpr float kClickDurationSec = 0.08f;
static constexpr float kSampleRate = 44100.0f;

static std::vector<AudioDevice> devices_;

// ── Playback state ──────────────────────────────────────────────

static std::atomic<bool> is_playing_{false};

// PCM buffer for decoded audio
static std::vector<float> pcm_buffer_;
static std::atomic<size_t> pcm_read_pos_{0};
static size_t pcm_total_frames_ = 0;
static int pcm_channels_ = 1;
static float pcm_volume_ = 1.0f;
static AudioQueueRef playback_queue_ = nullptr;
static std::mutex playback_mutex_;

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

    UInt32 out_channels = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i)
      out_channels += list->mBuffers[i].mNumberChannels;
    if (out_channels == 0)
      continue;

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

    AudioObjectPropertyAddress name_prop{};
    name_prop.mSelector = kAudioObjectPropertyName;
    name_prop.mScope = kAudioObjectPropertyScopeGlobal;
    name_prop.mElement = kAudioObjectPropertyElementMain;

    CFStringRef name_ref = nullptr;
    UInt32 name_size = sizeof(CFStringRef);
    err = AudioObjectGetPropertyData(dev_id, &name_prop, 0, nullptr, &name_size,
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

// ── Route AudioQueue to selected device ─────────────────────────

static void route_to_selected_device(AudioQueueRef queue) {
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
}

// ── PTT click audio queue callback ──────────────────────────────

static void click_queue_cb(void *, AudioQueueRef queue, AudioQueueBufferRef) {
  AudioQueueStop(queue, false);
  AudioQueueDispose(queue, false);
}

// ── Playback audio queue callback ───────────────────────────────

static void playback_queue_cb(void *, AudioQueueRef queue,
                              AudioQueueBufferRef buf) {
  size_t pos = pcm_read_pos_.load();
  size_t total_samples = pcm_total_frames_ * pcm_channels_;
  size_t frames_requested =
      buf->mAudioDataBytesCapacity / (sizeof(float) * pcm_channels_);
  size_t samples_requested = frames_requested * pcm_channels_;

  if (pos >= total_samples) {
    // Done playing — fill silence and stop
    std::memset(buf->mAudioData, 0, buf->mAudioDataBytesCapacity);
    buf->mAudioDataByteSize = 0;
    AudioQueueStop(queue, false);
    is_playing_ = false;
    return;
  }

  size_t samples_avail = total_samples - pos;
  size_t samples_to_copy =
      (samples_avail < samples_requested) ? samples_avail : samples_requested;

  auto *out = static_cast<float *>(buf->mAudioData);
  for (size_t i = 0; i < samples_to_copy; ++i)
    out[i] = pcm_buffer_[pos + i] * pcm_volume_;

  // Zero-fill remainder if at end
  if (samples_to_copy < samples_requested) {
    std::memset(out + samples_to_copy, 0,
                (samples_requested - samples_to_copy) * sizeof(float));
  }

  buf->mAudioDataByteSize =
      static_cast<UInt32>(samples_requested * sizeof(float));
  pcm_read_pos_.store(pos + samples_to_copy);

  AudioQueueEnqueueBuffer(queue, buf, 0, nullptr);

  // Check if we've reached the end
  if (pos + samples_to_copy >= total_samples)
    is_playing_ = false;
}

// ── Playback done callback ──────────────────────────────────────

static void playback_property_cb(void *, AudioQueueRef queue,
                                 AudioQueuePropertyID prop_id) {
  if (prop_id == kAudioQueueProperty_IsRunning) {
    UInt32 is_running = 0;
    UInt32 size = sizeof(is_running);
    AudioQueueGetProperty(queue, kAudioQueueProperty_IsRunning, &is_running,
                          &size);
    if (!is_running) {
      is_playing_ = false;
      // Clean up on a different context to avoid deadlock
      AudioQueueDispose(queue, false);
      std::lock_guard<std::mutex> lock(playback_mutex_);
      playback_queue_ = nullptr;
    }
  }
}

// ── MP3 decode via AudioFile ────────────────────────────────────

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

static bool decode_mp3(const std::vector<uint8_t> &mp3_data,
                       std::vector<float> &out_pcm, int &out_channels,
                       double &out_sample_rate) {
  AudioFileReadContext ctx{mp3_data.data(), mp3_data.size()};

  AudioFileID audio_file = nullptr;
  OSStatus err = AudioFileOpenWithCallbacks(&ctx, audio_file_read_proc, nullptr,
                                            audio_file_get_size_proc, nullptr,
                                            kAudioFileMP3Type, &audio_file);
  if (err != noErr) {
    char log[128];
    std::snprintf(log, sizeof(log),
                  "[xp_wellys_atc] AudioFileOpen failed: %d\n",
                  static_cast<int>(err));
    XPLMDebugString(log);
    return false;
  }

  // Get source format
  AudioStreamBasicDescription src_fmt{};
  UInt32 fmt_size = sizeof(src_fmt);
  err = AudioFileGetProperty(audio_file, kAudioFilePropertyDataFormat,
                             &fmt_size, &src_fmt);
  if (err != noErr) {
    AudioFileClose(audio_file);
    return false;
  }

  // Set up ExtAudioFile for conversion to float PCM
  ExtAudioFileRef ext_file = nullptr;
  err = ExtAudioFileWrapAudioFileID(audio_file, false, &ext_file);
  if (err != noErr) {
    AudioFileClose(audio_file);
    return false;
  }

  // Target format: float32 PCM
  AudioStreamBasicDescription dst_fmt{};
  dst_fmt.mSampleRate = src_fmt.mSampleRate;
  dst_fmt.mFormatID = kAudioFormatLinearPCM;
  dst_fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
  dst_fmt.mBitsPerChannel = 32;
  dst_fmt.mChannelsPerFrame = src_fmt.mChannelsPerFrame;
  dst_fmt.mBytesPerFrame = sizeof(float) * dst_fmt.mChannelsPerFrame;
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

  // Get total frame count
  SInt64 total_frames = 0;
  UInt32 prop_size = sizeof(total_frames);
  err =
      ExtAudioFileGetProperty(ext_file, kExtAudioFileProperty_FileLengthFrames,
                              &prop_size, &total_frames);
  if (err != noErr || total_frames <= 0) {
    ExtAudioFileDispose(ext_file);
    AudioFileClose(audio_file);
    return false;
  }

  // Read all frames
  out_pcm.resize(static_cast<size_t>(total_frames) * src_fmt.mChannelsPerFrame);

  UInt32 frames_to_read = static_cast<UInt32>(total_frames);
  AudioBufferList buf_list{};
  buf_list.mNumberBuffers = 1;
  buf_list.mBuffers[0].mNumberChannels = dst_fmt.mChannelsPerFrame;
  buf_list.mBuffers[0].mDataByteSize =
      static_cast<UInt32>(out_pcm.size() * sizeof(float));
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

  // Trim to actual frames read
  out_pcm.resize(static_cast<size_t>(frames_to_read) *
                 src_fmt.mChannelsPerFrame);

  out_channels = static_cast<int>(src_fmt.mChannelsPerFrame);
  out_sample_rate = src_fmt.mSampleRate;

  char log[128];
  std::snprintf(log, sizeof(log),
                "[xp_wellys_atc] Decoded MP3: %u frames, %d ch, %.0f Hz\n",
                frames_to_read, out_channels, out_sample_rate);
  XPLMDebugString(log);

  return true;
}

// ── Lifecycle ────────────────────────────────────────────────────

void init() {
  refresh_devices();
  is_playing_ = false;
}

void stop() {
  std::lock_guard<std::mutex> lock(playback_mutex_);
  if (playback_queue_) {
    AudioQueueStop(playback_queue_, true);
    AudioQueueDispose(playback_queue_, true);
    playback_queue_ = nullptr;
  }
  is_playing_ = false;
  devices_.clear();
}

// ── PTT click ────────────────────────────────────────────────────

void play_ptt_click() {
  float volume = settings::volume();
  if (volume <= 0.0f)
    return;

  int num_samples = static_cast<int>(kSampleRate * kClickDurationSec);

  std::vector<int16_t> samples(num_samples);
  for (int i = 0; i < num_samples; ++i) {
    float t = static_cast<float>(i) / kSampleRate;
    float sine = std::sin(2.0f * static_cast<float>(M_PI) * kClickFreqHz * t);

    float env = 1.0f;
    float fade_in = 0.005f * kSampleRate;
    float fade_out = 0.01f * kSampleRate;
    if (static_cast<float>(i) < fade_in)
      env = static_cast<float>(i) / fade_in;
    else if (static_cast<float>(i) > static_cast<float>(num_samples) - fade_out)
      env =
          (static_cast<float>(num_samples) - static_cast<float>(i)) / fade_out;

    samples[i] = static_cast<int16_t>(sine * env * volume * 16000.0f);
  }

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
  OSStatus err = AudioQueueNewOutput(&fmt, click_queue_cb, nullptr, nullptr,
                                     nullptr, 0, &queue);
  if (err != noErr || !queue) {
    XPLMDebugString(
        "[xp_wellys_atc] Failed to create audio queue for PTT click\n");
    return;
  }

  route_to_selected_device(queue);

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

// ── MP3 playback ────────────────────────────────────────────────

void play(const std::vector<uint8_t> &mp3_data, float volume) {
  // Stop any current playback
  {
    std::lock_guard<std::mutex> lock(playback_mutex_);
    if (playback_queue_) {
      AudioQueueStop(playback_queue_, true);
      AudioQueueDispose(playback_queue_, true);
      playback_queue_ = nullptr;
    }
  }

  if (mp3_data.empty()) {
    XPLMDebugString("[xp_wellys_atc] play() called with empty data\n");
    is_playing_ = false;
    return;
  }

  // Decode MP3 → PCM
  double sample_rate = 0;
  if (!decode_mp3(mp3_data, pcm_buffer_, pcm_channels_, sample_rate)) {
    XPLMDebugString("[xp_wellys_atc] MP3 decode failed\n");
    is_playing_ = false;
    return;
  }

  pcm_total_frames_ = pcm_buffer_.size() / pcm_channels_;
  pcm_read_pos_ = 0;
  pcm_volume_ = volume;
  is_playing_ = true;

  // Set up AudioQueue for float PCM output
  AudioStreamBasicDescription fmt{};
  fmt.mSampleRate = sample_rate;
  fmt.mFormatID = kAudioFormatLinearPCM;
  fmt.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
  fmt.mBitsPerChannel = 32;
  fmt.mChannelsPerFrame = static_cast<UInt32>(pcm_channels_);
  fmt.mBytesPerFrame = sizeof(float) * fmt.mChannelsPerFrame;
  fmt.mFramesPerPacket = 1;
  fmt.mBytesPerPacket = fmt.mBytesPerFrame;

  AudioQueueRef queue = nullptr;
  OSStatus err = AudioQueueNewOutput(&fmt, playback_queue_cb, nullptr, nullptr,
                                     nullptr, 0, &queue);
  if (err != noErr || !queue) {
    char log[128];
    std::snprintf(log, sizeof(log),
                  "[xp_wellys_atc] AudioQueueNewOutput failed: %d\n",
                  static_cast<int>(err));
    XPLMDebugString(log);
    is_playing_ = false;
    return;
  }

  // Listen for queue stop
  AudioQueueAddPropertyListener(queue, kAudioQueueProperty_IsRunning,
                                playback_property_cb, nullptr);

  route_to_selected_device(queue);

  {
    std::lock_guard<std::mutex> lock(playback_mutex_);
    playback_queue_ = queue;
  }

  // Allocate and prime 3 buffers (standard triple-buffering)
  static constexpr int kNumBuffers = 3;
  static constexpr UInt32 kFramesPerBuffer = 4096;
  UInt32 buf_size = kFramesPerBuffer * fmt.mBytesPerFrame;

  for (int i = 0; i < kNumBuffers; ++i) {
    AudioQueueBufferRef buf = nullptr;
    err = AudioQueueAllocateBuffer(queue, buf_size, &buf);
    if (err != noErr || !buf)
      continue;
    // Prime the buffer by calling the callback directly
    playback_queue_cb(nullptr, queue, buf);
  }

  err = AudioQueueStart(queue, nullptr);
  if (err != noErr) {
    char log[128];
    std::snprintf(log, sizeof(log),
                  "[xp_wellys_atc] AudioQueueStart failed: %d\n",
                  static_cast<int>(err));
    XPLMDebugString(log);
    std::lock_guard<std::mutex> lock(playback_mutex_);
    AudioQueueDispose(queue, true);
    playback_queue_ = nullptr;
    is_playing_ = false;
  }
}

bool is_playing() { return is_playing_.load(); }

} // namespace audio_player
