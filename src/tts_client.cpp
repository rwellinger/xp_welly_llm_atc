#include "tts_client.hpp"
#include "settings.hpp"

#include <XPLMUtilities.h>
#include <curl/curl.h>
#include <json.hpp>

#include <atomic>
#include <mutex>
#include <queue>
#include <thread>

namespace tts_client {

static std::mutex queue_mutex_;
static std::queue<std::function<void()>> callback_queue_;
static std::atomic<bool> stopped_{false};

static void enqueue_callback(std::function<void()> fn) {
  if (stopped_)
    return;
  std::lock_guard<std::mutex> lock(queue_mutex_);
  callback_queue_.push(std::move(fn));
}

static size_t write_binary_callback(char *ptr, size_t size, size_t nmemb,
                                    void *userdata) {
  auto *buffer = static_cast<std::vector<uint8_t> *>(userdata);
  size_t bytes = size * nmemb;
  buffer->insert(buffer->end(), reinterpret_cast<uint8_t *>(ptr),
                 reinterpret_cast<uint8_t *>(ptr) + bytes);
  return bytes;
}

void init() { stopped_ = false; }

void stop() {
  stopped_ = true;
  std::lock_guard<std::mutex> lock(queue_mutex_);
  std::queue<std::function<void()>> empty;
  std::swap(callback_queue_, empty);
}

void speak_async(
    const std::string &text,
    std::function<void(std::vector<uint8_t> mp3_data, bool success)> callback) {

  // NOLINTNEXTLINE(bugprone-exception-escape)
  std::thread([text, callback = std::move(callback)]() {
    std::string api_key = settings::get_api_key();
    if (api_key.empty()) {
      XPLMDebugString("[xp_wellys_atc] TTS error: no API key\n");
      enqueue_callback([callback]() { callback({}, false); });
      return;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
      XPLMDebugString("[xp_wellys_atc] TTS error: curl init failed\n");
      enqueue_callback([callback]() { callback({}, false); });
      return;
    }

    // Build JSON body (nlohmann handles escaping)
    nlohmann::json j;
    j["model"] = settings::tts_model();
    j["input"] = text;
    j["voice"] = settings::tts_voice();
    j["response_format"] = "mp3";
    std::string body = j.dump();

    // Headers
    std::string auth_header = "Authorization: Bearer " + api_key;
    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, auth_header.c_str());
    headers = curl_slist_append(headers, "Content-Type: application/json");

    // Response buffer — raw MP3 binary
    std::vector<uint8_t> mp3_data;

    curl_easy_setopt(curl, CURLOPT_URL,
                     "https://api.openai.com/v1/audio/speech");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_binary_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &mp3_data);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);

    CURLcode res = curl_easy_perform(curl);

    if (res != CURLE_OK) {
      std::string err = curl_easy_strerror(res);
      XPLMDebugString(
          ("[xp_wellys_atc][ERROR] TTS curl error: " + err + "\n").c_str());
      curl_slist_free_all(headers);
      curl_easy_cleanup(curl);
      enqueue_callback([callback]() { callback({}, false); });
      return;
    }

    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (http_code != 200) {
      // Response body is error JSON, not MP3
      std::string err_body(mp3_data.begin(), mp3_data.end());
      XPLMDebugString(("[xp_wellys_atc][ERROR] TTS HTTP " +
                       std::to_string(http_code) + ": " + err_body + "\n")
                          .c_str());
      enqueue_callback([callback]() { callback({}, false); });
      return;
    }

    char log[128];
    std::snprintf(log, sizeof(log),
                  "[xp_wellys_atc] TTS received %zu bytes MP3\n",
                  mp3_data.size());
    XPLMDebugString(log);

    enqueue_callback([callback, mp3_data = std::move(mp3_data)]() mutable {
      callback(std::move(mp3_data), true);
    });
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

} // namespace tts_client
