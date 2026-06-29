/*
 * xp_wellys_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 */

#include "backends/mistral_stt.hpp"

#include "backends/openai_common.hpp"
#include "core/logging.hpp"
#include "persistence/models_catalog.hpp"
#include "persistence/settings.hpp"

#include <curl/curl.h>
#include <json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <sstream>
#include <thread>
#include <utility>

namespace backends {

namespace {
constexpr const char *kBackendTag = "STT-MISTRAL";

size_t write_to_string(char *ptr, size_t size, size_t nmemb, void *userdata) {
  auto *response = static_cast<std::string *>(userdata);
  const size_t bytes = size * nmemb;
  response->append(ptr, bytes);
  return bytes;
}

// Split airport_context on commas and whitespace, drop empty tokens.
// Each non-empty token becomes a separate context_bias[] multipart
// field. Keeps short, distinct tokens (ICAO code, runway, city) as
// independent bias hints rather than one long string.
// Mistral requires each context_bias entry to match ^[^,\s]+$, so a
// single token must not contain whitespace either.
// Return true if the transcript is a Voxtral hallucination loop — a single
// word repeated more than kMaxConsecutive times in a row (e.g. "climb" ×100).
// Threshold of 5 avoids false positives on normal aviation repetition
// ("say again, say again, say again" = 3).
bool is_loop_hallucination(const std::string &text) {
  constexpr int kMaxConsecutive = 5;
  std::istringstream ss(text);
  std::string tok, prev;
  int run = 0;
  while (ss >> tok) {
    // lowercase + strip trailing punctuation for comparison
    std::transform(tok.begin(), tok.end(), tok.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    while (!tok.empty() && std::ispunct(static_cast<unsigned char>(tok.back())))
      tok.pop_back();
    if (tok.empty())
      continue;
    run = (tok == prev) ? run + 1 : 1;
    if (run > kMaxConsecutive)
      return true;
    prev = tok;
  }
  return false;
}

std::vector<std::string> split_context(const std::string &s) {
  std::vector<std::string> out;
  size_t lo = 0;
  for (size_t i = 0; i <= s.size(); ++i) {
    const bool at_end = (i == s.size());
    const bool is_delim =
        !at_end &&
        (s[i] == ',' || std::isspace(static_cast<unsigned char>(s[i])));
    if (at_end || is_delim) {
      if (i > lo)
        out.emplace_back(s.substr(lo, i - lo));
      lo = i + 1;
    }
  }
  return out;
}
} // namespace

MistralStt::MistralStt(std::string api_key, std::string model,
                       std::string base_url)
    : api_key_(std::move(api_key)), model_(std::move(model)),
      base_url_(std::move(base_url)) {}

std::string MistralStt::transcribe(const std::vector<float> &pcm_16k_mono,
                                   const std::string &airport_context) {
  last_error_.clear();
  if (api_key_.empty()) {
    logging::error("[%s] No API key configured", kBackendTag);
    last_error_ = std::string(kBackendTag) + ": No API key configured";
    return {};
  }
  if (pcm_16k_mono.empty())
    return {};

  std::string language = settings::backend_language();
  if (language.empty())
    language = "en";

  std::vector<uint8_t> wav = openai_common::pcm_float32_to_wav(pcm_16k_mono);
  const std::string key_tail = openai_common::last4(api_key_);
  logging::info("[%s] POST /v1/audio/transcriptions, %zu samples, model %s, "
                "lang=%s, key ...%s",
                kBackendTag, pcm_16k_mono.size(), model_.c_str(),
                language.c_str(), key_tail.c_str());

  CURL *curl = curl_easy_init();
  if (!curl) {
    logging::error("[%s] curl_easy_init failed", kBackendTag);
    last_error_ = std::string(kBackendTag) + ": curl_easy_init failed";
    return {};
  }

  curl_mime *mime = curl_mime_init(curl);

  curl_mimepart *part = curl_mime_addpart(mime);
  curl_mime_name(part, "file");
  curl_mime_data(part, reinterpret_cast<const char *>(wav.data()), wav.size());
  curl_mime_filename(part, "audio.wav");
  curl_mime_type(part, "audio/wav");

  part = curl_mime_addpart(mime);
  curl_mime_name(part, "model");
  curl_mime_data(part, model_.c_str(), CURL_ZERO_TERMINATED);

  part = curl_mime_addpart(mime);
  curl_mime_name(part, "language");
  curl_mime_data(part, language.c_str(), CURL_ZERO_TERMINATED);

  // Freeform prompt — accepted by all Voxtral models (standard whisper API
  // field). Sends the full aviation vocabulary + NATO phonetic callsign as
  // coherent text, which is far more effective than individual context_bias
  // tokens for phrase-level disambiguation (Romeo vs Remote, Mode vs not).
  if (!airport_context.empty()) {
    part = curl_mime_addpart(mime);
    curl_mime_name(part, "prompt");
    curl_mime_data(part, airport_context.c_str(), CURL_ZERO_TERMINATED);
  }

  if (models_catalog::mistral_stt_supports_context_bias(model_)) {
    for (const std::string &token : split_context(airport_context)) {
      part = curl_mime_addpart(mime);
      curl_mime_name(part, "context_bias[]");
      curl_mime_data(part, token.c_str(), CURL_ZERO_TERMINATED);
    }
  }

  const std::string url = base_url_ + "/v1/audio/transcriptions";
  const std::string auth = "Authorization: Bearer " + api_key_;
  struct curl_slist *headers = curl_slist_append(nullptr, auth.c_str());

  std::string response_body;
  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
  openai_common::apply_default_timeouts(curl, openai_common::CallKind::Stt);

  openai_common::HttpResult res;
  for (int attempt = 0; attempt < 2; ++attempt) {
    response_body.clear();
    CURLcode rc = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    res = openai_common::interpret(static_cast<int>(rc), http_code,
                                   response_body, kBackendTag);
    if (res.success || !res.transient)
      break;
    logging::info("[%s] transient error, retrying once: %s", kBackendTag,
                  res.error_message.c_str());
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }

  curl_mime_free(mime);
  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);

  if (!res.success) {
    logging::error("[%s] %s", kBackendTag, res.error_message.c_str());
    last_error_ = res.error_message;
    return {};
  }

  try {
    const auto j = nlohmann::json::parse(response_body);
    const std::string text = j.value("text", std::string{});
    if (is_loop_hallucination(text)) {
      logging::error("[%s] hallucination loop detected, rejecting transcript",
                     kBackendTag);
      last_error_ = std::string(kBackendTag) + ": hallucination loop rejected";
      return {};
    }
    return text;
  } catch (const std::exception &e) {
    logging::error("[%s] JSON parse error: %s", kBackendTag, e.what());
    last_error_ = std::string(kBackendTag) + ": JSON parse error: " + e.what();
    return {};
  }
}

} // namespace backends
