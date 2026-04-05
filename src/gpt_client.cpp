#include "gpt_client.hpp"
#include "settings.hpp"

#include <XPLMUtilities.h>
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

  std::string system_prompt =
      "You are an ATC controller at " + airport +
      " airport. "
      "The pilot is flying VFR. Respond using standard ICAO phraseology only. "
      "Plain text, no markdown. Maximum 2 sentences. "
      "The pilot's callsign is " +
      callsign +
      ". "
      "Current conditions: on ground=" +
      (on_ground ? "true" : "false") + ".";

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
        XPLMDebugString(
            ("[xp_wellys_atc][ERROR] GPT curl error: " + err + "\n").c_str());
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
        XPLMDebugString(("[xp_wellys_atc][ERROR] GPT HTTP " +
                         std::to_string(http_code) + ": " + response_body +
                         "\n")
                            .c_str());
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
