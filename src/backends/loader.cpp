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
#include "persistence/settings.hpp"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <sys/stat.h>
#include <thread>
#include <unordered_set>
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

// One ITextToSpeech survives across loader runs so newly-downloaded
// optional voices can be hot-loaded. Created on first run.
std::shared_ptr<PiperTts> g_piper;

void update_state(const model_manifest::Entry &entry, FileState s,
                  std::string message = {}) {
  std::lock_guard<std::mutex> lk(g_mtx);
  for (auto &f : g_status.files) {
    if (f.kind == entry.kind && f.voice_id == entry.voice_id) {
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
    g_status.files.push_back({e.kind, e.voice_id, FileState::NotChecked, {}});
  }
}

// True if the corresponding inference backend is currently registered
// with the manager. For voice entries this asks PiperTts whether the
// specific voice_id is loaded.
bool entry_loaded(const model_manifest::Entry &e) {
  switch (e.kind) {
  case model_manifest::Kind::WhisperModel:
    return backends::stt_ready();
  case model_manifest::Kind::LlamaModel:
    return backends::lm_ready();
  case model_manifest::Kind::PiperVoice:
  case model_manifest::Kind::PiperVoiceConfig:
    return g_piper && g_piper->has_voice(e.voice_id);
  }
  return false;
}

bool dir_exists(const std::string &path) {
  struct stat st{};
  if (stat(path.c_str(), &st) != 0)
    return false;
  return S_ISDIR(st.st_mode);
}

// The set of voice_ids the user currently wants loaded — i.e. the
// four roles' assignments. Optional voices not assigned to any role
// are not loaded into memory but still verified (so the UI can show
// them as Ready).
std::unordered_set<std::string> assigned_voice_ids() {
  using R = model_manifest::VoiceRole;
  std::unordered_set<std::string> v;
  for (auto role : model_manifest::all_roles())
    v.insert(settings::voice_for_role(role));
  (void)R{};
  return v;
}

// Walks the manifest, fast-checks size, computes SHA256 only for
// files that pass the size check. Updates `g_status` as it goes so
// the UI can reflect "Verifying… llama-3.2-3B (45%)" etc.
//
// Returns true if all *required* entries (Whisper, Llama, plus the
// four assigned voices' .onnx + .json) reach Verified. Optional
// voices that are missing do not count against this gate.
bool verify_files() {
  bool all_required_ok = true;
  auto wanted_voices = assigned_voice_ids();

  for (const auto &e : model_manifest::all()) {
    if (g_should_exit.load())
      return false;

    bool is_required = !e.optional;
    bool is_assigned_voice =
        (e.kind == model_manifest::Kind::PiperVoice ||
         e.kind == model_manifest::Kind::PiperVoiceConfig) &&
        wanted_voices.count(e.voice_id) > 0;
    bool counts_against_gate = is_required || is_assigned_voice;

    std::string full_path = model_paths::models_dir() + "/" + e.filename;

    if (!model_manifest::size_matches(e, full_path)) {
      // Distinguish "missing" vs "wrong size" so the UI can show
      // a useful error: missing == download me; wrong size ==
      // partial download / disk-corruption.
      struct stat st{};
      if (stat(full_path.c_str(), &st) != 0) {
        update_state(e, FileState::Missing, "File not found at " + full_path);
      } else {
        update_state(e, FileState::SizeMismatch,
                     "Size mismatch (have " +
                         std::to_string(static_cast<uint64_t>(st.st_size)) +
                         ", expected " + std::to_string(e.size_bytes) +
                         "). Likely a partial download - re-download to fix.");
      }
      if (counts_against_gate)
        all_required_ok = false;
      continue;
    }

    update_state(e, FileState::Verifying, "Computing SHA256...");
    std::string actual = model_manifest::sha256_file(full_path);

    if (g_should_exit.load())
      return false;

    if (actual.empty()) {
      update_state(e, FileState::Missing, "Failed to read " + full_path);
      if (counts_against_gate)
        all_required_ok = false;
      continue;
    }
    if (actual != e.sha256_hex) {
      update_state(e, FileState::HashMismatch,
                   "SHA256 mismatch (file is corrupt or modified). "
                   "Delete and re-download.");
      if (counts_against_gate)
        all_required_ok = false;
      continue;
    }
    update_state(e, FileState::Verified, {});
  }
  return all_required_ok;
}

// Open the three concrete backends in sequence. Whisper goes first
// because its model load is fastest and surfaces obvious problems
// (path errors, broken Metal cache) before we commit to the
// 2 GB llama load.
void load_backends() {
  using K = model_manifest::Kind;

  // Whisper
  if (!backends::stt_ready()) {
    const auto &whisper_entry = model_manifest::get(K::WhisperModel);
    update_state(whisper_entry, FileState::Loading,
                 "Loading whisper.cpp context...");
    auto stt = std::make_unique<backends::WhisperStt>();
    std::string p = model_paths::models_dir() + "/" + whisper_entry.filename;
    if (stt->open(p)) {
      backends::register_stt(std::move(stt));
      update_state(whisper_entry, FileState::Ready, {});
      logging::info("STT backend ready (whisper.cpp)");
    } else {
      update_state(whisper_entry, FileState::LoadError,
                   "whisper.cpp rejected the model file. Try re-downloading.");
      logging::error("Whisper open failed for %s", p.c_str());
    }
  }

  if (g_should_exit.load())
    return;

  // Llama
  if (!backends::lm_ready()) {
    const auto &llama_entry = model_manifest::get(K::LlamaModel);
    update_state(llama_entry, FileState::Loading,
                 "Loading llama.cpp context (this can take a few seconds)...");
    auto lm = std::make_unique<backends::LlamaLm>();
    std::string p = model_paths::models_dir() + "/" + llama_entry.filename;
    if (lm->open(p)) {
      backends::register_lm(std::move(lm));
      update_state(llama_entry, FileState::Ready, {});
      logging::info("LM backend ready (llama.cpp)");
    } else {
      update_state(llama_entry, FileState::LoadError,
                   "llama.cpp rejected the model file. Try re-downloading.");
      logging::error("Llama open failed for %s", p.c_str());
    }
  }

  if (g_should_exit.load())
    return;

  // Piper voices — load every voice currently assigned to a role
  // (voice files for those roles are required to be Verified;
  // verify_files() ensured this is the case before we got here).
  const std::string &espeak_dir = model_paths::espeakng_data_dir();
  if (!dir_exists(espeak_dir)) {
    const std::string msg = "espeak-ng-data missing at " + espeak_dir +
                            ". Reinstall the plugin bundle.";
    for (const auto &e : model_manifest::all()) {
      if (e.kind == K::PiperVoice || e.kind == K::PiperVoiceConfig)
        update_state(e, FileState::LoadError, msg);
    }
    logging::error("%s", msg.c_str());
    return;
  }

  // Lazy-create the Piper instance on the first successful load —
  // we keep one across loader re-runs so a hot-loaded optional voice
  // does not need to drag the others through unload/reload.
  bool piper_was_fresh = false;
  if (!g_piper) {
    g_piper = std::make_shared<PiperTts>();
    if (!g_piper->init(espeak_dir)) {
      logging::error("PiperTts init failed");
      g_piper.reset();
      return;
    }
    piper_was_fresh = true;
  }

  auto wanted_voices = assigned_voice_ids();
  bool any_voice_loaded = false;
  for (const std::string &voice_id : wanted_voices) {
    if (g_should_exit.load())
      return;
    if (g_piper->has_voice(voice_id)) {
      any_voice_loaded = true;
      continue;
    }

    const auto *onnx = model_manifest::get_voice(K::PiperVoice, voice_id);
    const auto *json = model_manifest::get_voice(K::PiperVoiceConfig, voice_id);
    if (!onnx || !json) {
      logging::error("Manifest mismatch: voice %s not found in catalog",
                     voice_id.c_str());
      continue;
    }
    update_state(*onnx, FileState::Loading, "Loading Piper voice...");
    update_state(*json, FileState::Loading, "Loading Piper voice config...");

    const std::string voice_path =
        model_paths::models_dir() + "/" + onnx->filename;
    const std::string config_path =
        model_paths::models_dir() + "/" + json->filename;

    if (g_piper->load_voice(voice_id, voice_path, config_path)) {
      update_state(*onnx, FileState::Ready, {});
      update_state(*json, FileState::Ready, {});
      any_voice_loaded = true;
      logging::info("Piper voice loaded: %s", voice_id.c_str());
    } else {
      const std::string msg = "Piper rejected " + voice_id;
      update_state(*onnx, FileState::LoadError, msg);
      update_state(*json, FileState::LoadError, msg);
      logging::error("%s", msg.c_str());
    }
  }

  if (any_voice_loaded && piper_was_fresh) {
    // Hand the manager its TTS pointer. We use shared_ptr internally
    // but the manager owns the unique_ptr — we transfer a fresh
    // unique_ptr that aliases through a custom deleter, except simpler:
    // since we only register once (piper_was_fresh), wrap a unique_ptr
    // around a copy of the shared_ptr's payload. That is unsafe;
    // instead, register a thin shim that holds the shared_ptr.
    //
    // Pragmatic fix: stash the shared_ptr in a unique_ptr-friendly
    // wrapper. The simplest is a custom deleter that drops the
    // shared_ptr ref count.
    struct PiperShim final : ITextToSpeech {
      std::shared_ptr<PiperTts> inner;
      explicit PiperShim(std::shared_ptr<PiperTts> p) : inner(std::move(p)) {}
      bool load_voice(const std::string &voice_id,
                      const std::string &voice_onnx_path,
                      const std::string &voice_json_path) override {
        return inner->load_voice(voice_id, voice_onnx_path, voice_json_path);
      }
      void unload_voice(const std::string &voice_id) override {
        inner->unload_voice(voice_id);
      }
      bool has_voice(const std::string &voice_id) const override {
        return inner->has_voice(voice_id);
      }
      std::vector<int16_t> synthesize(const std::string &voice_id,
                                      const std::string &text,
                                      float length_scale,
                                      uint32_t &sample_rate_hz) override {
        return inner->synthesize(voice_id, text, length_scale, sample_rate_hz);
      }
    };
    backends::register_tts(std::make_unique<PiperShim>(g_piper));
    logging::info("TTS backend ready (Piper)");
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
  if (!backends::stt_ready() || !backends::lm_ready() || !backends::tts_ready())
    return false;
  // Every entry that counts against the readiness gate (required
  // entries + the four assigned voices' .onnx and .json) must be
  // Ready.
  std::unordered_set<std::string> wanted;
  for (auto role : model_manifest::all_roles())
    wanted.insert(settings::voice_for_role(role));
  for (const auto &f : files) {
    bool is_voice_kind = (f.kind == model_manifest::Kind::PiperVoice ||
                          f.kind == model_manifest::Kind::PiperVoiceConfig);
    if (is_voice_kind && wanted.count(f.voice_id) == 0)
      continue; // optional voice not assigned anywhere
    if (f.state != FileState::Ready)
      return false;
  }
  return true;
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
      // Find the corresponding manifest entry to feed entry_loaded.
      const model_manifest::Entry *e = nullptr;
      for (const auto &cand : model_manifest::all()) {
        if (cand.kind == f.kind && cand.voice_id == f.voice_id) {
          e = &cand;
          break;
        }
      }
      if (e && f.state == FileState::Ready && entry_loaded(*e))
        continue;
      f.state = FileState::NotChecked;
      f.message.clear();
    }
  }

  // Join any prior thread before launching a new one.
  if (g_worker.joinable())
    g_worker.join();
  g_worker = std::thread(run_worker);
}

void stop() {
  g_should_exit = true;
  if (g_worker.joinable()) {
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
  g_piper.reset();
}

std::string espeakng_data_dir_for_piper() {
  return model_paths::espeakng_data_dir();
}

} // namespace backends::loader
