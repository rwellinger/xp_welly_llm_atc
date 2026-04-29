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

#include "openai/gpt_client.hpp"
#include "atc_templates.hpp"
#include "logging.hpp"
#include "settings.hpp"

#include <curl/curl.h>
#include <json.hpp>

#include <atomic>
#include <mutex>
#include <queue>
#include <thread>

namespace gpt_client {

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

void init() { stopped_ = false; }

void stop() { stopped_ = true; }

void ask_async(
    const std::string &pilot_text, const xplane_context::XPlaneContext &ctx,
    std::function<void(std::string response, bool success)> callback) {

  // Capture context data by value for the thread
  std::string airport =
      ctx.nearest_airport_id.empty() ? "unknown" : ctx.nearest_airport_id;
  std::string callsign = settings::pilot_callsign();
  bool on_ground = ctx.on_ground;
  std::string model = settings::gpt_model();

  // Prefer region-specific prompt if present (gpt_fallback_prompt_us for US,
  // otherwise fall back to gpt_fallback_prompt).
  std::string system_prompt;
  if (settings::flow_region() == "US")
    system_prompt = atc_templates::get_prompt("gpt_fallback_prompt_us");
  if (system_prompt.empty())
    system_prompt = atc_templates::get_prompt("gpt_fallback_prompt");
  if (system_prompt.empty()) {
    const bool us = (settings::flow_region() == "US");
    system_prompt =
        std::string(
            "You are an ATC controller at {airport} airport. The pilot is "
            "flying VFR. Respond using standard ") +
        (us ? "FAA/NAV CANADA phraseology" : "ICAO phraseology") +
        " only. Plain text, no markdown. Maximum 2 sentences. The pilot's "
        "callsign is {callsign}. Current conditions: on "
        "ground={on_ground}.";
  }
  system_prompt = atc_templates::fill(
      system_prompt, {{"airport", airport},
                      {"callsign", callsign},
                      {"on_ground", on_ground ? "true" : "false"}});

  if (settings::debug_logging()) {
    logging::debug("GPT request (fallback) ---");
    logging::debug("  system: %s", system_prompt.c_str());
    logging::debug("  user: %s", pilot_text.c_str());
  }

  // NOLINTNEXTLINE(bugprone-exception-escape)
  std::thread([pilot_text, system_prompt, model,
               callback = std::move(callback)]() {
    try {
      std::string api_key = settings::get_api_key();
      if (api_key.empty()) {
        enqueue_callback(
            [callback]() { callback("No API key configured", false); });
        return;
      }

      CURL *curl = curl_easy_init();
      if (!curl) {
        enqueue_callback(
            [callback]() { callback("Failed to initialize curl", false); });
        return;
      }

      // Build JSON body
      nlohmann::json body = {{"model", model},
                             {"messages",
                              {{{"role", "system"}, {"content", system_prompt}},
                               {{"role", "user"}, {"content", pilot_text}}}},
                             {"max_tokens", 150},
                             {"temperature", 0.7}};

      std::string body_str = body.dump();

      // Headers
      std::string auth_header = "Authorization: Bearer " + api_key;
      struct curl_slist *headers = nullptr;
      headers = curl_slist_append(headers, auth_header.c_str());
      headers = curl_slist_append(headers, "Content-Type: application/json");

      std::string response_body;

      curl_easy_setopt(curl, CURLOPT_URL,
                       "https://api.openai.com/v1/chat/completions");
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
      curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

      CURLcode res = curl_easy_perform(curl);

      if (res != CURLE_OK) {
        std::string err = curl_easy_strerror(res);
        logging::error("GPT curl error: %s", err.c_str());
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        enqueue_callback(
            [callback, err]() { callback("Curl error: " + err, false); });
        return;
      }

      long http_code = 0;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

      curl_slist_free_all(headers);
      curl_easy_cleanup(curl);

      if (http_code != 200) {
        logging::error("GPT HTTP %ld: %s", http_code, response_body.c_str());
        std::string err =
            "HTTP " + std::to_string(http_code) + ": " + response_body;
        enqueue_callback([callback, err]() { callback(err, false); });
        return;
      }

      try {
        auto j = nlohmann::json::parse(response_body);
        std::string content =
            j["choices"][0]["message"]["content"].get<std::string>();
        enqueue_callback([callback, content]() { callback(content, true); });
      } catch (const std::exception &e) {
        std::string err = std::string("JSON parse error: ") + e.what();
        enqueue_callback([callback, err]() { callback(err, false); });
      }
    } catch (...) { // NOLINT(bugprone-empty-catch)
    }
  }).detach();
}

void classify_intent_async(
    const std::string &transcript, const std::string &system_prompt,
    std::function<void(std::string intent_key, bool success)> callback) {

  if (settings::debug_logging()) {
    logging::debug("GPT request (classify_intent) ---");
    logging::debug("  system: %s", system_prompt.c_str());
    logging::debug("  user: %s", transcript.c_str());
  }

  // NOLINTNEXTLINE(bugprone-exception-escape)
  std::thread([transcript, system_prompt, callback = std::move(callback)]() {
    try {
      std::string api_key = settings::get_api_key();
      if (api_key.empty()) {
        enqueue_callback([callback]() { callback("_INVALID", false); });
        return;
      }

      CURL *curl = curl_easy_init();
      if (!curl) {
        enqueue_callback([callback]() { callback("_INVALID", false); });
        return;
      }

      nlohmann::json body = {{"model", "gpt-4o-mini"},
                             {"messages",
                              {{{"role", "system"}, {"content", system_prompt}},
                               {{"role", "user"}, {"content", transcript}}}},
                             {"max_tokens", 20},
                             {"temperature", 0.0}};

      std::string body_str = body.dump();

      std::string auth_header = "Authorization: Bearer " + api_key;
      struct curl_slist *headers = nullptr;
      headers = curl_slist_append(headers, auth_header.c_str());
      headers = curl_slist_append(headers, "Content-Type: application/json");

      std::string response_body;

      curl_easy_setopt(curl, CURLOPT_URL,
                       "https://api.openai.com/v1/chat/completions");
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body_str.c_str());
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_body);
      curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

      CURLcode res = curl_easy_perform(curl);

      if (res != CURLE_OK) {
        std::string err = curl_easy_strerror(res);
        logging::error("GPT classify curl error: %s", err.c_str());
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        enqueue_callback([callback]() { callback("_INVALID", false); });
        return;
      }

      long http_code = 0;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

      curl_slist_free_all(headers);
      curl_easy_cleanup(curl);

      if (http_code != 200) {
        logging::error("GPT classify HTTP %ld", http_code);
        enqueue_callback([callback]() { callback("_INVALID", false); });
        return;
      }

      try {
        auto j = nlohmann::json::parse(response_body);
        std::string content =
            j["choices"][0]["message"]["content"].get<std::string>();
        // Trim whitespace
        while (!content.empty() && std::isspace(content.front()))
          content.erase(content.begin());
        while (!content.empty() && std::isspace(content.back()))
          content.pop_back();
        enqueue_callback([callback, content]() { callback(content, true); });
      } catch (const std::exception &e) {
        logging::error("GPT classify parse error: %s", e.what());
        enqueue_callback([callback]() { callback("_INVALID", false); });
      }
    } catch (...) { // NOLINT(bugprone-empty-catch)
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

} // namespace gpt_client
