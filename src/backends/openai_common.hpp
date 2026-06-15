/*
 * xp_wellys_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 *
 * Shared helpers for the three OpenAI cloud clients (STT / LM / TTS).
 * Lives in the plugin module — the engine OBJECT lib must stay
 * SDK-free and libcurl-free. Cloud-only TU: never included from any
 * local backend or from the engine.
 */

#ifndef BACKENDS_OPENAI_COMMON_HPP
#define BACKENDS_OPENAI_COMMON_HPP

#include <cstdint>
#include <string>
#include <vector>

// Forward-declare CURL so callers that already include <curl/curl.h>
// (which is every cloud-backend .cpp) match the same opaque pointer.
typedef void CURL;

namespace backends::openai_common {

// Default endpoint root; constructor-injectable so unit tests can point
// at a local mock HTTP server.
inline constexpr const char *kDefaultBaseUrl = "https://api.openai.com";

// Which kind of cloud call we're making. Drives the default read
// timeout — TTS needs more headroom than STT/LM because the server
// streams audio after generating it, which takes noticeably longer.
enum class CallKind { Stt, Lm, Tts };

// Apply the standard timeout set to `curl`:
//   CURLOPT_CONNECTTIMEOUT = 5  (fail fast on no-network)
//   CURLOPT_TIMEOUT        = 30 (Stt/Lm)  or  45 (Tts)
// Called once per request after curl_easy_init().
void apply_default_timeouts(CURL *curl, CallKind kind);

// Result of a single curl_easy_perform() attempt, plus a flag that
// tells the caller whether retrying makes sense.
struct HttpResult {
  long http_code = 0;
  int curl_code = 0; // CURLcode; kept as int to avoid pulling in curl.h
  bool success = false;
  bool transient = false;    // retry-worthy: timeout, conn reset, 5xx, 429
  std::string error_message; // human-readable summary for the UI banner
};

// Build an HttpResult from the (rc, http_code, response_body) triple
// returned by a curl_easy_perform() call. Sets `transient=true` for
// network-level errors (timeout, connect refused, broken pipe) and for
// HTTP 5xx / 429 — caller can retry once. `backend_tag` is the audit
// log tag of the calling backend, used to prefix the error message
// (e.g. "STT-OPENAI: timeout after 30s").
HttpResult interpret(int curl_code, long http_code,
                     const std::string &response_body, const char *backend_tag);

// Return the last 4 characters of `api_key` (or the whole string if
// shorter). Used in audit-log lines so we identify which key was used
// without ever leaking the full secret.
std::string last4(const std::string &api_key);

// Encode 16 kHz mono float PCM (range [-1, 1]) as a minimal WAV file
// (44-byte canonical PCM header + 16-bit samples). The output is what
// OpenAI's /v1/audio/transcriptions accepts as multipart "file" field.
std::vector<uint8_t> pcm_float32_to_wav(const std::vector<float> &pcm_16k_mono);

// Decode a canonical PCM WAV file (16-bit mono) into raw int16 samples
// and report the sample rate. Returns an empty vector on parse failure
// or non-16-bit-PCM input.
std::vector<int16_t> wav_to_pcm_int16(const std::vector<uint8_t> &wav,
                                      uint32_t &sample_rate_hz);

} // namespace backends::openai_common

#endif // BACKENDS_OPENAI_COMMON_HPP
