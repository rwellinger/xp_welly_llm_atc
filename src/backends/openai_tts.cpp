/*
 * xp_wellys_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 */

#include "backends/openai_tts.hpp"

#include "core/logging.hpp"

#include <curl/curl.h>
#include <json.hpp>

#include <algorithm>
#include <chrono>
#include <thread>
#include <utility>

namespace backends {

namespace {
constexpr const char *kBackendTag = "TTS-OPENAI";

bool is_valid_openai_voice(const std::string &v) {
  return v == "alloy" || v == "echo" || v == "fable" || v == "onyx" ||
         v == "nova" || v == "shimmer";
}

size_t write_to_vec(char *ptr, size_t size, size_t nmemb, void *userdata) {
  auto *buf = static_cast<std::vector<uint8_t> *>(userdata);
  const size_t bytes = size * nmemb;
  buf->insert(buf->end(), reinterpret_cast<uint8_t *>(ptr),
              reinterpret_cast<uint8_t *>(ptr) + bytes);
  return bytes;
}
} // namespace

OpenAiTts::OpenAiTts(std::string api_key, std::string model,
                     std::string base_url)
    : api_key_(std::move(api_key)), model_(std::move(model)),
      base_url_(std::move(base_url)) {}

bool OpenAiTts::load_voice(const std::string &voice_id, const std::string &,
                           const std::string &) {
  if (!is_valid_openai_voice(voice_id)) {
    logging::error("[%s] Unknown OpenAI voice id: %s", kBackendTag,
                   voice_id.c_str());
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  loaded_voices_.insert(voice_id);
  return true;
}

void OpenAiTts::unload_voice(const std::string &voice_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  loaded_voices_.erase(voice_id);
}

bool OpenAiTts::has_voice(const std::string &voice_id) const {
  std::lock_guard<std::mutex> lock(mutex_);
  return loaded_voices_.count(voice_id) > 0;
}

std::string OpenAiTts::default_voice_for(model_manifest::VoiceRole role) const {
  using R = model_manifest::VoiceRole;
  switch (role) {
  case R::Atis:
    return "onyx";
  case R::Tower:
    return "echo";
  case R::Ground:
    return "alloy";
  case R::Center:
    return "echo";
  }
  return "onyx";
}

std::vector<int16_t> OpenAiTts::synthesize(const std::string &voice_id,
                                           const std::string &text,
                                           float length_scale,
                                           uint32_t &sample_rate_hz) {
  sample_rate_hz = 0;
  last_error_.clear();
  if (api_key_.empty()) {
    logging::error("[%s] No API key configured", kBackendTag);
    last_error_ = std::string(kBackendTag) + ": No API key configured";
    return {};
  }
  if (!is_valid_openai_voice(voice_id)) {
    logging::error("[%s] Unknown OpenAI voice id: %s", kBackendTag,
                   voice_id.c_str());
    last_error_ = std::string(kBackendTag) + ": Unknown voice id: " + voice_id;
    return {};
  }
  if (text.empty())
    return {};

  // Piper's length_scale > 1 means "slower". OpenAI's speed > 1 means
  // "faster". Invert and clamp to OpenAI's accepted range [0.25, 4.0].
  const float raw_speed = (length_scale > 0.0f) ? (1.0f / length_scale) : 1.0f;
  const float speed = std::max(0.25f, std::min(4.0f, raw_speed));

  const std::string key_tail = openai_common::last4(api_key_);
  logging::info("[%s] POST /v1/audio/speech, voice %s, %zu chars, speed %.2f, "
                "key sk-...%s",
                kBackendTag, voice_id.c_str(), text.size(), speed,
                key_tail.c_str());

  nlohmann::json body = {
      {"model", model_},          {"input", text},
      {"voice", voice_id},        {"speed", speed},
      {"response_format", "wav"},
  };
  const std::string body_str = body.dump();

  CURL *curl = curl_easy_init();
  if (!curl) {
    logging::error("[%s] curl_easy_init failed", kBackendTag);
    last_error_ = std::string(kBackendTag) + ": curl_easy_init failed";
    return {};
  }

  const std::string url = base_url_ + "/v1/audio/speech";
  const std::string auth = "Authorization: Bearer " + api_key_;
  struct curl_slist *headers = curl_slist_append(nullptr, auth.c_str());
  headers = curl_slist_append(headers, "Content-Type: application/json");

  std::vector<uint8_t> wav_bytes;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_vec);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &wav_bytes);
  openai_common::apply_default_timeouts(curl, openai_common::CallKind::Tts);

  openai_common::HttpResult res;
  for (int attempt = 0; attempt < 2; ++attempt) {
    wav_bytes.clear();
    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    // For the HTTP-error path we want the body in the interpret()
    // summary — wav_bytes carries it on non-200 responses, but we
    // can't pass it as a string without copying. Pass an excerpt.
    const size_t excerpt_len = std::min<size_t>(wav_bytes.size(), 160);
    const std::string body_excerpt(
        reinterpret_cast<const char *>(wav_bytes.data()), excerpt_len);
    res = openai_common::interpret(static_cast<int>(rc), http_code,
                                   body_excerpt, kBackendTag);
    if (res.success || !res.transient)
      break;
    logging::info("[%s] transient error, retrying once: %s", kBackendTag,
                  res.error_message.c_str());
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (!res.success) {
    logging::error("[%s] %s", kBackendTag, res.error_message.c_str());
    last_error_ = res.error_message;
    return {};
  }

  std::vector<int16_t> pcm =
      openai_common::wav_to_pcm_int16(wav_bytes, sample_rate_hz);
  if (pcm.empty() || sample_rate_hz == 0) {
    logging::error("[%s] WAV decode failed (%zu bytes received)", kBackendTag,
                   wav_bytes.size());
    last_error_ = std::string(kBackendTag) + ": WAV decode failed";
    return {};
  }
  return pcm;
}

} // namespace backends
