/*
 * xp_wellys_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 */

#include "backends/mistral_tts.hpp"

#include "backends/openai_common.hpp"
#include "core/logging.hpp"
#include "persistence/settings.hpp"

#include <curl/curl.h>
#include <json.hpp>

#include <utility>

namespace backends {

namespace {
constexpr const char *kBackendTag = "TTS-MISTRAL";

size_t write_to_vec(char *ptr, size_t size, size_t nmemb, void *userdata) {
  auto *buf = static_cast<std::vector<uint8_t> *>(userdata);
  const size_t bytes = size * nmemb;
  buf->insert(buf->end(), reinterpret_cast<uint8_t *>(ptr),
              reinterpret_cast<uint8_t *>(ptr) + bytes);
  return bytes;
}
} // namespace

MistralTts::MistralTts(std::string api_key, std::string model,
                       std::string base_url)
    : api_key_(std::move(api_key)), model_(std::move(model)),
      base_url_(std::move(base_url)) {}

bool MistralTts::load_voice(const std::string &voice_id, const std::string &,
                            const std::string &) {
  // Mistral has no client-side voice whitelist — accept any non-empty
  // id and remember it for has_voice() lookups. An empty id is a
  // configuration error (user did not paste a Voxtral voice id into
  // Settings yet); surface that as a load failure rather than caching
  // an empty entry that would later confuse synthesize().
  if (voice_id.empty()) {
    logging::info("[%s] load_voice called with empty voice id (set one in "
                  "Settings to enable TTS)",
                  kBackendTag);
    return false;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  loaded_voices_.insert(voice_id);
  return true;
}

void MistralTts::unload_voice(const std::string &voice_id) {
  std::lock_guard<std::mutex> lock(mutex_);
  loaded_voices_.erase(voice_id);
}

bool MistralTts::has_voice(const std::string &voice_id) const {
  if (voice_id.empty())
    return false;
  std::lock_guard<std::mutex> lock(mutex_);
  return loaded_voices_.count(voice_id) > 0;
}

std::string
MistralTts::default_voice_for(model_manifest::VoiceRole role) const {
  using R = model_manifest::VoiceRole;
  switch (role) {
  case R::Atis:
    return settings::mistral_tts_voice_atis();
  case R::Tower:
    return settings::mistral_tts_voice_tower();
  case R::Ground:
    return settings::mistral_tts_voice_ground();
  case R::Center:
    return settings::mistral_tts_voice_tower();
  }
  return {};
}

std::vector<int16_t> MistralTts::synthesize(const std::string &voice_id,
                                            const std::string &text,
                                            float length_scale,
                                            uint32_t &sample_rate_hz) {
  // length_scale has no direct equivalent in Voxtral TTS — ATIS will
  // still synthesize, just at the model's native rate. We log the
  // value so it shows up once in Log.txt and the user understands
  // what was dropped.
  (void)length_scale;

  sample_rate_hz = 0;
  if (api_key_.empty()) {
    logging::error("[%s] No API key configured", kBackendTag);
    return {};
  }
  if (voice_id.empty()) {
    logging::error("[%s] Empty voice id — open Settings and paste a Voxtral "
                   "voice id",
                   kBackendTag);
    return {};
  }
  if (text.empty())
    return {};

  const std::string key_tail = openai_common::last4(api_key_);
  logging::info("[%s] POST /v1/audio/speech, voice %s, %zu chars, "
                "key sk-...%s",
                kBackendTag, voice_id.c_str(), text.size(), key_tail.c_str());

  nlohmann::json body = {
      {"input", text},
      {"voice_id", voice_id},
      {"response_format", "wav"},
  };
  // Only include `model` when the user actually set one — Voxtral TTS
  // picks a sensible server-side default for the empty case, which
  // keeps the Settings UI tidy (one fewer string to paste).
  if (!model_.empty())
    body["model"] = model_;
  const std::string body_str = body.dump();

  CURL *curl = curl_easy_init();
  if (!curl) {
    logging::error("[%s] curl_easy_init failed", kBackendTag);
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
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

  const CURLcode rc = curl_easy_perform(curl);
  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (rc != CURLE_OK) {
    logging::error("[%s] curl error: %s", kBackendTag, curl_easy_strerror(rc));
    return {};
  }
  if (http_code != 200) {
    const std::string err(wav_bytes.begin(), wav_bytes.end());
    logging::error("[%s] HTTP %ld: %s", kBackendTag, http_code, err.c_str());
    return {};
  }

  std::vector<int16_t> pcm =
      openai_common::wav_to_pcm_int16(wav_bytes, sample_rate_hz);
  if (pcm.empty() || sample_rate_hz == 0) {
    logging::error("[%s] WAV decode failed (%zu bytes received)", kBackendTag,
                   wav_bytes.size());
    return {};
  }
  return pcm;
}

} // namespace backends
