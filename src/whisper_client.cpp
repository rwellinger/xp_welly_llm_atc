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

#include "whisper_client.hpp"
#include "atc_templates.hpp"
#include "settings.hpp"

#include <XPLMUtilities.h>
#include <curl/curl.h>
#include <json.hpp>

#include <atomic>
#include <mutex>
#include <queue>
#include <thread>

namespace whisper_client {

static std::mutex queue_mutex_;
static std::queue<std::function<void()>> callback_queue_;
static std::atomic<bool> stopped_{false};

static void enqueue_callback(std::function<void()> fn) {
  if (stopped_)
    return;
  std::lock_guard<std::mutex> lock(queue_mutex_);
  callback_queue_.push(std::move(fn));
}

static size_t write_callback(char *ptr, size_t size, size_t nmemb,
                             void *userdata) {
  auto *response = static_cast<std::string *>(userdata);
  response->append(ptr, size * nmemb);
  return size * nmemb;
}

void init() {
  curl_global_init(CURL_GLOBAL_DEFAULT);
  stopped_ = false;
}

void stop() {
  stopped_ = true;
  curl_global_cleanup();
}

void transcribe_async(
    std::vector<uint8_t> wav_data,
    std::function<void(TranscriptResult result)> callback,
    const std::string &airport_context) {

  // NOLINTNEXTLINE(bugprone-exception-escape)
  std::thread([wav_data = std::move(wav_data),
               callback = std::move(callback),
               airport_context]() {
    std::string api_key = settings::get_api_key();
    if (api_key.empty()) {
      enqueue_callback([callback]() {
        callback({"No API key configured", 0.0f, false});
      });
      return;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
      enqueue_callback([callback]() {
        callback({"Failed to initialize curl", 0.0f, false});
      });
      return;
    }

    // Build multipart form
    curl_mime *mime = curl_mime_init(curl);

    curl_mimepart *part = curl_mime_addpart(mime);
    curl_mime_name(part, "file");
    curl_mime_data(part, reinterpret_cast<const char *>(wav_data.data()),
                   wav_data.size());
    curl_mime_filename(part, "audio.wav");
    curl_mime_type(part, "audio/wav");

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "model");
    curl_mime_data(part, "whisper-1", CURL_ZERO_TERMINATED);

    part = curl_mime_addpart(mime);
    curl_mime_name(part, "language");
    curl_mime_data(part, "en", CURL_ZERO_TERMINATED);

    // Request verbose JSON for quality metrics (no_speech_prob, avg_logprob)
    part = curl_mime_addpart(mime);
    curl_mime_name(part, "response_format");
    curl_mime_data(part, "verbose_json", CURL_ZERO_TERMINATED);

    // Prompt hint to guide Whisper toward aviation phraseology
    std::string whisper_prompt = atc_templates::get_prompt("whisper_prompt");
    if (!airport_context.empty()) {
      whisper_prompt = "Airport: " + airport_context + ". " + whisper_prompt;
    }
    if (!whisper_prompt.empty()) {
      part = curl_mime_addpart(mime);
      curl_mime_name(part, "prompt");
      curl_mime_data(part, whisper_prompt.c_str(), CURL_ZERO_TERMINATED);
    }

    // Auth header
    std::string auth_header = "Authorization: Bearer " + api_key;
    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, auth_header.c_str());

    // Response buffer
    std::string response_body;

    curl_easy_setopt(curl, CURLOPT_URL,
                     "https://api.openai.com/v1/audio/transcriptions");
    curl_easy_setopt(curl, CURLOPT_MIMEPOST, mime);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
      std::string err;
      if (res == CURLE_OPERATION_TIMEDOUT) {
        err = "[Error: transcription timed out]";
        XPLMDebugString("[xp_wellys_atc][ERROR] Whisper API timeout (>15s)\n");
      } else {
        err = "Curl error: " + std::string(curl_easy_strerror(res));
        XPLMDebugString(
            ("[xp_wellys_atc][ERROR] Whisper curl error: " + err + "\n")
                .c_str());
      }
      curl_mime_free(mime);
      curl_slist_free_all(headers);
      curl_easy_cleanup(curl);
      enqueue_callback([callback, err]() {
        callback({err, 0.0f, false});
      });
      return;
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_mime_free(mime);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (http_code != 200) {
      std::string err = "[Error: transcription failed]";
      XPLMDebugString(("[xp_wellys_atc][ERROR] Whisper HTTP " +
                       std::to_string(http_code) + ": " + response_body + "\n")
                          .c_str());
      enqueue_callback([callback, err]() {
        callback({err, 0.0f, false});
      });
      return;
    }

    try {
      auto j = nlohmann::json::parse(response_body);
      std::string transcript = j.value("text", "");

      // Compute quality score from verbose_json segment data
      float quality = 1.0f;
      if (j.contains("segments") && j["segments"].is_array()) {
        float worst_no_speech = 0.0f;
        float sum_logprob = 0.0f;
        int seg_count = 0;
        for (const auto &seg : j["segments"]) {
          float nsp = seg.value("no_speech_prob", 0.0f);
          float alp = seg.value("avg_logprob", 0.0f);
          if (nsp > worst_no_speech)
            worst_no_speech = nsp;
          sum_logprob += alp;
          ++seg_count;
        }
        float avg_logprob =
            seg_count > 0 ? sum_logprob / static_cast<float>(seg_count)
                          : -1.0f;

        // no_speech_prob: 0=speech, 1=silence/noise
        // avg_logprob: 0=perfect, -1+=uncertain (typical range -0.2 to -1.0)
        // Quality = (1 - no_speech) * logprob_factor
        float speech_factor = 1.0f - worst_no_speech;
        float logprob_factor =
            std::min(1.0f, std::max(0.0f, (avg_logprob + 1.0f)));
        quality = speech_factor * logprob_factor;

        if (settings::debug_logging()) {
          char dbg[256];
          std::snprintf(
              dbg, sizeof(dbg),
              "[xp_wellys_atc][DEBUG] Whisper quality: %.2f "
              "(no_speech=%.2f, avg_logprob=%.2f, segments=%d)\n",
              quality, worst_no_speech, avg_logprob, seg_count);
          XPLMDebugString(dbg);
        }
      }

      TranscriptResult result{transcript, quality, true};
      enqueue_callback(
          [callback, result]() { callback(result); });
    } catch (const std::exception &e) {
      std::string err = std::string("JSON parse error: ") + e.what();
      enqueue_callback([callback, err]() {
        callback({err, 0.0f, false});
      });
    }
  }).detach();
}

void drain_callback_queue() {
  std::queue<std::function<void()>> local_queue;
  {
    std::lock_guard<std::mutex> lock(queue_mutex_);
    std::swap(local_queue, callback_queue_);
  }
  while (!local_queue.empty()) {
    local_queue.front()();
    local_queue.pop();
  }
}

} // namespace whisper_client
