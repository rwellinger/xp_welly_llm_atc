/*
 * xp_wellys_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 */

#include "backends/loader.hpp"

#include "backends/llama_lm.hpp"
#include "backends/manager.hpp"
#include "backends/piper_tts.hpp"
#include "backends/whisper_stt.hpp"
#include "core/logging.hpp"
#include "persistence/model_paths.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <utility>

namespace backends::loader {

namespace {

// ── Shared state ────────────────────────────────────────────────────
//
// `g_status` is read from the main thread (UI snapshot) and written
// by the verification worker. A single mutex covers the whole struct
// — updates are coarse-grained (per state transition) so contention
// is negligible.
std::mutex g_mtx;
Status g_status; // protected by g_mtx

// `g_running` ensures only one verification worker exists at a time.
// `g_should_exit` lets stop() signal the worker to bail mid-SHA256.
std::atomic<bool> g_running{false};
std::atomic<bool> g_should_exit{false};
std::thread g_worker;

void update_state(model_manifest::Kind kind, FileState s,
                  std::string message = {}) {
  std::lock_guard<std::mutex> lk(g_mtx);
  for (auto &f : g_status.files) {
    if (f.kind == kind) {
      f.state = s;
      f.message = std::move(message);
      return;
    }
  }
}

void seed_status_locked() {
  // Initial layout, mirrors the manifest order. Idempotent: only
  // overwritten on a fresh start() — re-runs preserve any Ready
  // state that was already established.
  if (!g_status.files.empty())
    return;
  for (const auto &e : model_manifest::all()) {
    g_status.files.push_back({e.kind, FileState::NotChecked, {}});
  }
}

// True if the backend is currently registered with the manager. The
// loader keeps state in lockstep with the manager's view; if a Ready
// file's backend somehow became unregistered (manager::stop), we let
// start() re-load it.
bool backend_registered_for(model_manifest::Kind kind) {
  switch (kind) {
  case model_manifest::Kind::WhisperModel:
    return backends::stt_ready();
  case model_manifest::Kind::LlamaModel:
    return backends::lm_ready();
  case model_manifest::Kind::PiperVoice:
  case model_manifest::Kind::PiperVoiceConfig:
    return backends::tts_ready();
  }
  return false;
}

bool dir_exists(const std::string &path) {
  struct stat st{};
  if (stat(path.c_str(), &st) != 0)
    return false;
  return S_ISDIR(st.st_mode);
}

// Walks the manifest, fast-checks size, computes SHA256 only for
// files that pass the size check. Updates `g_status` as it goes so
// the UI can reflect "Verifying… llama-3.2-3B (45%)" etc. — for now
// the granularity is per-file (Verifying → Verified|HashMismatch).
//
// Returns true if all four entries reach Verified.
bool verify_files() {
  bool all_ok = true;
  for (const auto &e : model_manifest::all()) {
    if (g_should_exit.load())
      return false;

    std::string full_path = model_paths::models_dir() + "/" + e.filename;

    if (!model_manifest::size_matches(e, full_path)) {
      // Distinguish "missing" vs "wrong size" so the UI can show
      // a useful error: missing == download me; wrong size ==
      // partial download / disk-corruption.
      struct stat st{};
      if (stat(full_path.c_str(), &st) != 0) {
        update_state(e.kind, FileState::Missing,
                     "File not found at " + full_path);
      } else {
        update_state(e.kind, FileState::SizeMismatch,
                     "Size mismatch (have " +
                         std::to_string(static_cast<uint64_t>(st.st_size)) +
                         ", expected " + std::to_string(e.size_bytes) +
                         "). Likely a partial download - re-download to fix.");
      }
      all_ok = false;
      continue;
    }

    update_state(e.kind, FileState::Verifying, "Computing SHA256...");
    std::string actual = model_manifest::sha256_file(full_path);

    if (g_should_exit.load())
      return false;

    if (actual.empty()) {
      update_state(e.kind, FileState::Missing, "Failed to read " + full_path);
      all_ok = false;
      continue;
    }
    if (actual != e.sha256_hex) {
      update_state(e.kind, FileState::HashMismatch,
                   "SHA256 mismatch (file is corrupt or modified). "
                   "Delete and re-download.");
      all_ok = false;
      continue;
    }
    update_state(e.kind, FileState::Verified, {});
  }
  return all_ok;
}

// Open the three concrete backends in sequence. Whisper goes first
// because its model load is fastest and surfaces obvious problems
// (path errors, broken Metal cache) before we commit to the
// 2 GB llama load.
void load_backends() {
  using K = model_manifest::Kind;

  // Whisper
  {
    update_state(K::WhisperModel, FileState::Loading,
                 "Loading whisper.cpp context...");
    auto stt = std::make_unique<backends::WhisperStt>();
    std::string p = model_paths::models_dir() + "/" +
                    model_manifest::get(K::WhisperModel).filename;
    if (stt->open(p)) {
      backends::register_stt(std::move(stt));
      update_state(K::WhisperModel, FileState::Ready, {});
      logging::info("STT backend ready (whisper.cpp)");
    } else {
      update_state(K::WhisperModel, FileState::LoadError,
                   "whisper.cpp rejected the model file. Try re-downloading.");
      logging::error("Whisper open failed for %s", p.c_str());
    }
  }

  if (g_should_exit.load())
    return;

  // Llama
  {
    update_state(K::LlamaModel, FileState::Loading,
                 "Loading llama.cpp context (this can take a few seconds)...");
    auto lm = std::make_unique<backends::LlamaLm>();
    std::string p = model_paths::models_dir() + "/" +
                    model_manifest::get(K::LlamaModel).filename;
    if (lm->open(p)) {
      backends::register_lm(std::move(lm));
      update_state(K::LlamaModel, FileState::Ready, {});
      logging::info("LM backend ready (llama.cpp)");
    } else {
      update_state(K::LlamaModel, FileState::LoadError,
                   "llama.cpp rejected the model file. Try re-downloading.");
      logging::error("Llama open failed for %s", p.c_str());
    }
  }

  if (g_should_exit.load())
    return;

  // Piper (one backend, two manifest entries — voice + config)
  {
    update_state(K::PiperVoice, FileState::Loading, "Loading Piper voice...");
    update_state(K::PiperVoiceConfig, FileState::Loading,
                 "Loading Piper voice config...");

    const std::string voice_path = model_paths::models_dir() + "/" +
                                   model_manifest::get(K::PiperVoice).filename;
    const std::string config_path =
        model_paths::models_dir() + "/" +
        model_manifest::get(K::PiperVoiceConfig).filename;
    const std::string &espeak_dir = model_paths::espeakng_data_dir();

    // espeak-ng-data ships with the plugin; if it is missing the
    // build / install pipeline shipped a broken bundle and Piper
    // cannot work. Surface a distinct error so the user knows it
    // is a packaging bug, not a missing download.
    if (!dir_exists(espeak_dir)) {
      const std::string msg = "espeak-ng-data missing at " + espeak_dir +
                              ". Reinstall the plugin bundle.";
      update_state(K::PiperVoice, FileState::LoadError, msg);
      update_state(K::PiperVoiceConfig, FileState::LoadError, msg);
      logging::error("%s", msg.c_str());
      return;
    }

    auto tts = std::make_unique<backends::PiperTts>();
    if (tts->open(voice_path, config_path, espeak_dir)) {
      backends::register_tts(std::move(tts));
      update_state(K::PiperVoice, FileState::Ready, {});
      update_state(K::PiperVoiceConfig, FileState::Ready, {});
      logging::info("TTS backend ready (Piper)");
    } else {
      const std::string msg =
          "Piper rejected the voice / config / espeak-ng-data triple.";
      update_state(K::PiperVoice, FileState::LoadError, msg);
      update_state(K::PiperVoiceConfig, FileState::LoadError, msg);
      logging::error("%s", msg.c_str());
    }
  }
}

void run_worker() {
  // Guard against any std::filesystem / whisper.cpp / llama.cpp /
  // Piper exception escaping into std::thread destructor and
  // terminating X-Plane. We log + leave g_running false so a
  // subsequent start() can retry.
  try {
    bool all_files_verified = verify_files();
    if (g_should_exit.load()) {
      g_running = false;
      return;
    }
    if (all_files_verified) {
      load_backends();
    } else {
      logging::info(
          "One or more model files are missing or corrupt - backends not "
          "loaded; open the plugin window to download.");
    }
  } catch (const std::exception &e) {
    logging::error("loader: run_worker threw: %s", e.what());
  } catch (...) {
    logging::error("loader: run_worker threw an unknown exception");
  }
  g_running = false;
}

} // namespace

bool Status::all_ready() const {
  for (const auto &f : files) {
    if (f.state != FileState::Ready)
      return false;
  }
  // Backed by the manager's view. If the manager dropped a backend
  // since we declared it Ready (manager::stop), we are not actually
  // ready.
  return backends::stt_ready() && backends::lm_ready() && backends::tts_ready();
}

Status snapshot() {
  std::lock_guard<std::mutex> lk(g_mtx);
  return g_status;
}

void start() {
  // If a worker is already running, leave it alone — start() is a
  // hint, not a force.
  bool expected = false;
  if (!g_running.compare_exchange_strong(expected, true))
    return;

  g_should_exit = false;
  {
    std::lock_guard<std::mutex> lk(g_mtx);
    seed_status_locked();
    // Reset transient states; preserve Ready entries whose backend
    // is still registered so a re-run after a download doesn't
    // spuriously reload the others.
    for (auto &f : g_status.files) {
      if (f.state == FileState::Ready && backend_registered_for(f.kind))
        continue;
      f.state = FileState::NotChecked;
      f.message.clear();
    }
  }

  // Join any prior thread before launching a new one. CTAS above
  // ensured no other thread can be entering run_worker, but the prior
  // run_worker may not have been join()ed yet.
  if (g_worker.joinable())
    g_worker.join();
  g_worker = std::thread(run_worker);
}

void stop() {
  g_should_exit = true;
  if (g_worker.joinable()) {
    // Worker uses g_should_exit between SHA256 chunks and between
    // backend loads. SHA256 of the 2 GB llama file is ~4 s on M1
    // worst case. We wait up to ~6 s — same budget the manager
    // uses for in-flight inference workers.
    using clock = std::chrono::steady_clock;
    auto deadline = clock::now() + std::chrono::seconds(6);
    while (g_running.load() && clock::now() < deadline) {
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    if (g_worker.joinable())
      g_worker.join();
  }
  g_running = false;
  g_should_exit = false;
}

std::string espeakng_data_dir_for_piper() {
  return model_paths::espeakng_data_dir();
}

} // namespace backends::loader
