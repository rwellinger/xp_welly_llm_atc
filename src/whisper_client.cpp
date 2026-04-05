#include "whisper_client.hpp"
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
    std::function<void(std::string transcript, bool success)> callback) {

  std::thread([wav_data = std::move(wav_data),
               callback = std::move(callback)]() {
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
      enqueue_callback([callback, err]() { callback(err, false); });
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
      enqueue_callback([callback, err]() { callback(err, false); });
      return;
    }

    try {
      auto j = nlohmann::json::parse(response_body);
      std::string transcript = j["text"].get<std::string>();
      enqueue_callback(
          [callback, transcript]() { callback(transcript, true); });
    } catch (const std::exception &e) {
      std::string err = std::string("JSON parse error: ") + e.what();
      enqueue_callback([callback, err]() { callback(err, false); });
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
