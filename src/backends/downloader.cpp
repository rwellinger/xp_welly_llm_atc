/*
 * xp_wellys_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 */

#include "backends/downloader.hpp"

#include "backends/loader.hpp"
#include "core/logging.hpp"
#include "persistence/model_manifest.hpp"
#include "persistence/model_paths.hpp"

#include <curl/curl.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>

namespace backends::downloader {

namespace {

namespace fs = std::filesystem;

// ── Per-entry state, shared between worker and UI ────────────────────
//
// We keep one Progress per manifest entry indexed by kind; UI calls
// snapshot() to copy them out under g_mtx.
std::mutex g_mtx;
std::vector<Progress> g_progress; // protected by g_mtx, mirrors manifest order

// FIFO of pending downloads. Worker pops the front under g_mtx.
std::deque<model_manifest::Kind> g_queue; // protected by g_mtx

// Worker lifecycle.
std::thread g_worker;
std::atomic<bool> g_worker_alive{false};
std::condition_variable g_wake;

// `g_active_kind` is the kind the worker is currently downloading.
// `g_cancel_active` is checked from libcurl's progress callback; the
// callback returning non-zero aborts the transfer with
// `CURLE_ABORTED_BY_CALLBACK`.
std::atomic<bool> g_cancel_active{false};
// Used to signal the worker to exit altogether (vs. just cancelling
// the current download).
std::atomic<bool> g_should_stop{false};

// Live-updated by libcurl's xferinfo callback — read by the UI on
// every frame to feed a progress bar without locking.
std::atomic<uint64_t> g_active_bytes_total{0};
std::atomic<uint64_t> g_active_bytes_downloaded{0};
std::atomic<int> g_active_kind_index{-1}; // -1 = no active download

void seed_progress_locked() {
  if (!g_progress.empty())
    return;
  for (const auto &e : model_manifest::all()) {
    g_progress.push_back({e.kind, State::Idle, e.size_bytes, 0, {}});
  }
}

Progress *find_progress_locked(model_manifest::Kind kind) {
  for (auto &p : g_progress) {
    if (p.kind == kind)
      return &p;
  }
  return nullptr;
}

void set_state(model_manifest::Kind kind, State s, std::string msg = {},
               uint64_t bytes_done = UINT64_MAX) {
  std::lock_guard<std::mutex> lk(g_mtx);
  if (auto *p = find_progress_locked(kind)) {
    p->state = s;
    p->error_message = std::move(msg);
    if (bytes_done != UINT64_MAX)
      p->bytes_downloaded = bytes_done;
  }
}

uint64_t file_size(const std::string &path) {
  struct stat st{};
  if (stat(path.c_str(), &st) != 0)
    return 0;
  if (!S_ISREG(st.st_mode))
    return 0;
  return static_cast<uint64_t>(st.st_size);
}

// libcurl write callback — append straight to the open .part file.
size_t write_to_file(char *buf, size_t size, size_t nmemb, void *user) {
  auto *fp = static_cast<std::FILE *>(user);
  return std::fwrite(buf, size, nmemb, fp);
}

// libcurl xferinfo callback — runs on the transfer thread (our worker
// here). curl_off_t is signed but always positive in practice; cast
// with care.
int xferinfo_cb(void * /*user*/, curl_off_t dltotal, curl_off_t dlnow,
                curl_off_t /*ultotal*/, curl_off_t /*ulnow*/) {
  if (dltotal > 0)
    g_active_bytes_total = static_cast<uint64_t>(dltotal);
  if (dlnow >= 0)
    g_active_bytes_downloaded = static_cast<uint64_t>(dlnow);
  // Returning non-zero aborts the transfer with
  // CURLE_ABORTED_BY_CALLBACK. We check both flags so a global stop
  // implies a current cancel.
  if (g_cancel_active.load() || g_should_stop.load())
    return 1;
  return 0;
}

// One-shot download attempt for a single manifest entry. Returns true
// on success (file present at final path, SHA256-verified), false
// otherwise (state already set in g_progress on failure paths).
bool download_one(const model_manifest::Entry &e) {
  const std::string final_path = model_paths::models_dir() + "/" + e.filename;
  const std::string part_path = final_path + ".part";

  // If the final file already exists with the right size + hash, we
  // are done. (loader.cpp may have already done this check; we redo
  // it here to make the downloader callable independently.)
  if (model_manifest::size_matches(e, final_path)) {
    if (model_manifest::sha256_file(final_path) == e.sha256_hex) {
      set_state(e.kind, State::Done, {}, e.size_bytes);
      return true;
    }
    // size matches but hash doesn't — fall through to redownload.
  }

  // Disk-space precheck. We need (size_bytes - bytes_already_present)
  // free, plus a small headroom for fs metadata.
  uint64_t resume_from = file_size(part_path);
  if (resume_from > e.size_bytes) {
    // Stale .part bigger than the manifest — nuke it.
    std::error_code ec;
    fs::remove(part_path, ec);
    resume_from = 0;
  }
  uint64_t need = e.size_bytes - resume_from + (1024ULL * 1024); // 1 MB slack

  std::error_code sec;
  fs::space_info si = fs::space(model_paths::models_dir(), sec);
  if (!sec) {
    if (si.available < need) {
      char msg[160];
      std::snprintf(msg, sizeof(msg),
                    "Need %.1f MB free; only %.1f MB available on this volume.",
                    static_cast<double>(need) / 1024.0 / 1024.0,
                    static_cast<double>(si.available) / 1024.0 / 1024.0);
      set_state(e.kind, State::InsufficientDisk, msg, resume_from);
      return false;
    }
  } // if `space` fails we let the actual write surface the error

  // Open .part in append-binary mode if resuming, write-binary
  // otherwise. fwrite is fine because libcurl's write callback only
  // appends — no seeks needed.
  std::FILE *fp = std::fopen(part_path.c_str(), resume_from > 0 ? "ab" : "wb");
  if (!fp) {
    set_state(e.kind, State::Failed,
              "Cannot open " + part_path + " for writing", resume_from);
    return false;
  }

  set_state(e.kind, State::Downloading, {}, resume_from);

  CURL *curl = curl_easy_init();
  if (!curl) {
    std::fclose(fp);
    set_state(e.kind, State::Failed, "curl_easy_init() failed", resume_from);
    return false;
  }

  // Reset per-transfer atomics so the UI shows progress only for the
  // active download.
  g_active_bytes_total = e.size_bytes;
  g_active_bytes_downloaded = resume_from;
  g_cancel_active = false;

  curl_easy_setopt(curl, CURLOPT_URL, e.url.c_str());
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
  curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_file);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
  curl_easy_setopt(curl, CURLOPT_FAILONERROR, 1L); // 4xx/5xx -> error
  // 0 disables the legacy progress callback; we use xferinfo (the
  // 64-bit replacement).
  curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
  curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, xferinfo_cb);
  curl_easy_setopt(curl, CURLOPT_XFERINFODATA, nullptr);
  // Resume from byte offset. CURLOPT_RESUME_FROM_LARGE handles >2 GB
  // correctly (Llama gguf is 1.88 GB but llama-405B etc. would need
  // 64-bit offsets).
  if (resume_from > 0)
    curl_easy_setopt(curl, CURLOPT_RESUME_FROM_LARGE,
                     static_cast<curl_off_t>(resume_from));
  // Reasonable timeouts — connect within 30 s; total download has no
  // timeout (Llama on slow ADSL is realistic).
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 30L);
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L); // 1 KB/s
  curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);    // for 60 s

  CURLcode rc = curl_easy_perform(curl);
  long http_code = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
  curl_easy_cleanup(curl);
  std::fclose(fp);

  if (rc == CURLE_ABORTED_BY_CALLBACK) {
    // Cancel — keep .part for resume.
    set_state(e.kind, State::Cancelled, "Cancelled by user",
              file_size(part_path));
    return false;
  }
  if (rc != CURLE_OK) {
    char msg[256];
    std::snprintf(msg, sizeof(msg), "Download failed (%s; HTTP %ld)",
                  curl_easy_strerror(rc), http_code);
    set_state(e.kind, State::Failed, msg, file_size(part_path));
    return false;
  }

  // Sanity: did we actually receive `size_bytes`? HTTP 200 with a
  // truncated body is rare but possible behind misbehaving proxies.
  uint64_t got = file_size(part_path);
  if (got != e.size_bytes) {
    char msg[160];
    std::snprintf(
        msg, sizeof(msg),
        "Wrong size after download (got %llu, expected %llu) - partial "
        "transfer; click Download again to resume.",
        static_cast<unsigned long long>(got),
        static_cast<unsigned long long>(e.size_bytes));
    set_state(e.kind, State::Failed, msg, got);
    return false;
  }

  set_state(e.kind, State::Verifying, {}, got);
  std::string actual = model_manifest::sha256_file(part_path);
  if (actual != e.sha256_hex) {
    // Hash mismatch is bad: either the upstream content moved (manifest
    // is stale) or the disk is corrupt. We delete the .part so the next
    // attempt starts fresh — resuming a corrupt file would never help.
    std::error_code ec;
    fs::remove(part_path, ec);
    set_state(e.kind, State::Failed,
              "SHA256 mismatch after download - file deleted. If this keeps "
              "happening, the upstream URL may have changed; check the "
              "README's manual-fallback table.",
              0);
    return false;
  }

  // Atomic rename. On the same filesystem this is a single inode op,
  // so a concurrent SHA256 read in the loader cannot see a partial
  // file. (The actual atomicity guarantee here comes from POSIX
  // rename(2), which std::filesystem::rename calls into.)
  std::error_code ec;
  fs::rename(part_path, final_path, ec);
  if (ec) {
    set_state(e.kind, State::Failed,
              "Could not rename .part to final filename: " + ec.message(), got);
    return false;
  }

  set_state(e.kind, State::Done, {}, e.size_bytes);
  logging::info("Downloaded + verified %s", e.filename.c_str());
  return true;
}

void worker_loop() {
  // Guard the entire worker against any std::filesystem / libcurl /
  // string exception escaping into the std::thread destructor — which
  // would otherwise call std::terminate() and take X-Plane down. We
  // log the error and surface it on the active row so the user sees
  // it in the Models tab.
  try {
    while (true) {
      model_manifest::Kind next;
      {
        std::unique_lock<std::mutex> lk(g_mtx);
        g_wake.wait(lk,
                    []() { return !g_queue.empty() || g_should_stop.load(); });
        if (g_should_stop.load())
          break;
        next = g_queue.front();
        g_queue.pop_front();

        // Update active-kind index for the UI's "active row" highlight.
        int idx = -1;
        for (size_t i = 0; i < g_progress.size(); ++i) {
          if (g_progress[i].kind == next) {
            idx = static_cast<int>(i);
            break;
          }
        }
        g_active_kind_index = idx;
      }

      const auto &entry = model_manifest::get(next);
      try {
        download_one(entry); // updates g_progress internally
      } catch (const std::exception &e) {
        set_state(entry.kind, State::Failed,
                  std::string("Internal error: ") + e.what(),
                  file_size(model_paths::models_dir() + "/" + entry.filename +
                            ".part"));
        logging::error("downloader: download_one threw: %s", e.what());
      } catch (...) {
        set_state(entry.kind, State::Failed,
                  "Internal error: unknown exception",
                  file_size(model_paths::models_dir() + "/" + entry.filename +
                            ".part"));
        logging::error("downloader: download_one threw an unknown exception");
      }

      g_active_kind_index = -1;
      g_active_bytes_total = 0;
      g_active_bytes_downloaded = 0;

      // After every successful download, poke the loader so the UI
      // shows "Verifying -> Ready" without the user clicking anything.
      // Even on failure we re-poke so a corrupt file flips back to
      // HashMismatch promptly. loader::start is best-effort; don't
      // bring down the worker if it throws.
      try {
        backends::loader::start();
      } catch (const std::exception &e) {
        logging::error("downloader: loader::start() threw: %s", e.what());
      } catch (...) {
        logging::error("downloader: loader::start() threw unknown");
      }
    }
  } catch (const std::exception &e) {
    logging::error("downloader: worker_loop threw at outer level: %s",
                   e.what());
  } catch (...) {
    logging::error("downloader: worker_loop threw unknown at outer level");
  }
  g_worker_alive = false;
}

void ensure_worker() {
  bool expected = false;
  if (!g_worker_alive.compare_exchange_strong(expected, true)) {
    // Already running — wake it in case it was waiting on an empty
    // queue.
    g_wake.notify_one();
    return;
  }
  g_should_stop = false;
  if (g_worker.joinable())
    g_worker.join();
  g_worker = std::thread(worker_loop);
}

} // namespace

std::vector<Progress> snapshot() {
  std::lock_guard<std::mutex> lk(g_mtx);
  seed_progress_locked();

  // Fold live atomics into the active row so the UI sees per-frame
  // progress without us writing under g_mtx in the xferinfo
  // callback.
  int active = g_active_kind_index.load();
  if (active >= 0 && active < static_cast<int>(g_progress.size())) {
    auto &p = g_progress[static_cast<size_t>(active)];
    if (p.state == State::Downloading) {
      p.bytes_total = g_active_bytes_total.load();
      p.bytes_downloaded = g_active_bytes_downloaded.load();
    }
  }
  return g_progress;
}

uint64_t free_space_bytes() {
  std::error_code ec;
  fs::space_info si = fs::space(model_paths::models_dir(), ec);
  if (ec)
    return 0;
  return si.available;
}

uint64_t bytes_still_required() {
  uint64_t total = 0;
  for (const auto &e : model_manifest::all()) {
    const std::string final_path = model_paths::models_dir() + "/" + e.filename;
    uint64_t have = file_size(final_path);
    // Also count an in-progress .part so the "still need" number
    // reflects what a resumed download will pull, not the full size.
    if (have == 0) {
      have = file_size(final_path + ".part");
    }
    if (have >= e.size_bytes)
      continue; // already complete (or larger; ignore)
    total += e.size_bytes - have;
  }
  return total;
}

void enqueue(model_manifest::Kind kind) {
  {
    std::lock_guard<std::mutex> lk(g_mtx);
    seed_progress_locked();

    // De-dup: skip if this kind is already queued or actively
    // downloading.
    for (auto k : g_queue)
      if (k == kind)
        return;
    int active = g_active_kind_index.load();
    if (active >= 0 && active < static_cast<int>(g_progress.size()) &&
        g_progress[static_cast<size_t>(active)].kind == kind) {
      return;
    }

    g_queue.push_back(kind);
    if (auto *p = find_progress_locked(kind)) {
      p->state = State::Queued;
      p->error_message.clear();
    }
  }
  ensure_worker();
}

size_t enqueue_all_missing() {
  size_t n = 0;
  // Read loader status first to know which kinds are not Ready.
  auto status = backends::loader::snapshot();
  for (const auto &fs : status.files) {
    using FS = backends::loader::FileState;
    if (fs.state == FS::Missing || fs.state == FS::SizeMismatch ||
        fs.state == FS::HashMismatch) {
      enqueue(fs.kind);
      ++n;
    }
  }
  return n;
}

void cancel(model_manifest::Kind kind) {
  bool was_active = false;
  {
    std::lock_guard<std::mutex> lk(g_mtx);
    // Drop from queue if not yet started.
    for (auto it = g_queue.begin(); it != g_queue.end();) {
      if (*it == kind) {
        it = g_queue.erase(it);
        if (auto *p = find_progress_locked(kind)) {
          p->state = State::Cancelled;
          p->error_message = "Cancelled before start";
        }
      } else {
        ++it;
      }
    }

    // If currently active, set the cancel flag — xferinfo_cb returns
    // non-zero on next callback fire (libcurl ticks at ~1/s by
    // default but typically more often during transfers).
    int active = g_active_kind_index.load();
    if (active >= 0 && active < static_cast<int>(g_progress.size()) &&
        g_progress[static_cast<size_t>(active)].kind == kind) {
      was_active = true;
    }
  }
  if (was_active)
    g_cancel_active = true;
}

void stop() {
  g_should_stop = true;
  g_cancel_active = true;
  g_wake.notify_all();

  if (g_worker.joinable()) {
    using clock = std::chrono::steady_clock;
    auto deadline = clock::now() + std::chrono::seconds(5);
    while (g_worker_alive.load() && clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (g_worker.joinable())
      g_worker.join();
  }
  g_worker_alive = false;
  g_should_stop = false;
  g_cancel_active = false;
  {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_queue.clear();
  }
}

} // namespace backends::downloader
