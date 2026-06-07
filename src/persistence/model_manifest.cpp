/*
 * xp_wellys_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 */

#include "persistence/model_manifest.hpp"

#include "persistence/models_catalog.hpp"

#include <CommonCrypto/CommonDigest.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace model_manifest {

namespace {

// Friendly display name for a Piper voice. Strips the locale prefix
// and turns underscores into spaces so the UI says "lessac (medium)"
// rather than "en_US-lessac-medium".
std::string voice_display_name(const std::string &voice_id) {
  std::string s = voice_id;
  size_t dash = s.find('-');
  if (dash != std::string::npos && dash <= 6)
    s = s.substr(dash + 1);
  return s;
}

// The manifest is rebuilt the first time it's accessed *after*
// models_catalog::init() ran, so a fresh catalog (loaded from JSON)
// flows through. We cache the result behind a flag so subsequent calls
// are cheap; settings::init() loads the catalog before anything else
// touches the manifest, so the cache is populated exactly once at boot.
std::mutex g_mtx;
std::vector<Entry> g_entries;
bool g_built = false;

void rebuild_locked() {
  g_entries.clear();
  const std::string &piper_base = models_catalog::piper_base_url();

  for (const auto &w : models_catalog::local_whisper_entries()) {
    g_entries.push_back({Kind::WhisperModel, w.filename, w.size_bytes, w.sha256,
                         w.url, w.display_name, /*voice_id=*/"",
                         /*optional=*/false, w.language});
  }
  for (const auto &l : models_catalog::local_llama_entries()) {
    g_entries.push_back({Kind::LlamaModel, l.filename, l.size_bytes, l.sha256,
                         l.url, l.display_name, /*voice_id=*/"",
                         /*optional=*/false, l.language});
  }
  for (const auto &row : models_catalog::local_piper_voices()) {
    const std::string onnx = row.voice_id + ".onnx";
    const std::string json = onnx + ".json";
    const std::string base = piper_base + "/" + row.url_subpath + "/";
    const std::string disp =
        "Piper voice (" + voice_display_name(row.voice_id) + ")";
    g_entries.push_back({Kind::PiperVoice, onnx, row.onnx_size, row.onnx_sha256,
                         base + onnx, disp, row.voice_id, row.optional,
                         row.language});
    g_entries.push_back({Kind::PiperVoiceConfig, json, row.json_size,
                         row.json_sha256, base + json, disp + " - config",
                         row.voice_id, row.optional, row.language});
  }
  g_built = true;
}

const std::vector<Entry> &manifest() {
  std::lock_guard<std::mutex> lk(g_mtx);
  if (!g_built)
    rebuild_locked();
  return g_entries;
}

} // namespace

const std::vector<VoiceRole> &all_roles() {
  static const std::vector<VoiceRole> v = {
      VoiceRole::Atis, VoiceRole::Tower, VoiceRole::Ground, VoiceRole::Center};
  return v;
}

const char *role_name(VoiceRole role) {
  switch (role) {
  case VoiceRole::Atis:
    return "Atis";
  case VoiceRole::Tower:
    return "Tower";
  case VoiceRole::Ground:
    return "Ground";
  case VoiceRole::Center:
    return "Center";
  }
  return "Unknown";
}

bool role_from_name(const std::string &name, VoiceRole &out) {
  if (name == "Atis" || name == "atis") {
    out = VoiceRole::Atis;
    return true;
  }
  if (name == "Tower" || name == "tower") {
    out = VoiceRole::Tower;
    return true;
  }
  if (name == "Ground" || name == "ground") {
    out = VoiceRole::Ground;
    return true;
  }
  if (name == "Center" || name == "center") {
    out = VoiceRole::Center;
    return true;
  }
  return false;
}

std::string entry_key(const Entry &e) {
  // Append the language tag for kinds that can have multiple
  // language-specific variants. Piper entries carry their language
  // implicitly via voice_id; Llama is language-agnostic.
  switch (e.kind) {
  case Kind::WhisperModel:
    return std::string("whisper:") + (e.language.empty() ? "any" : e.language);
  case Kind::LlamaModel:
    return "llama";
  case Kind::PiperVoice:
    return "voice:" + e.voice_id + ":onnx";
  case Kind::PiperVoiceConfig:
    return "voice:" + e.voice_id + ":json";
  }
  return {};
}

const std::vector<Entry> &all() { return manifest(); }

const Entry &get(Kind kind) {
  // Caller bug: voice kinds need a voice_id.
  if (kind == Kind::PiperVoice || kind == Kind::PiperVoiceConfig)
    std::abort();
  for (const auto &e : manifest()) {
    if (e.kind == kind)
      return e;
  }
  std::abort();
}

const Entry &get_for_language(Kind kind, const std::string &language) {
  // Caller bug: voice kinds need a voice_id, not a language.
  if (kind == Kind::PiperVoice || kind == Kind::PiperVoiceConfig)
    std::abort();
  const Entry *fallback = nullptr;
  for (const auto &e : manifest()) {
    if (e.kind != kind)
      continue;
    if (e.language == language)
      return e;
    if (e.language.empty())
      fallback = &e;
  }
  if (fallback)
    return *fallback;
  std::abort();
}

const Entry *get_voice(Kind kind, const std::string &voice_id) {
  if (kind != Kind::PiperVoice && kind != Kind::PiperVoiceConfig)
    return nullptr;
  for (const auto &e : manifest()) {
    if (e.kind == kind && e.voice_id == voice_id)
      return &e;
  }
  return nullptr;
}

const std::vector<std::string> &voice_ids() {
  // Re-derived from the live catalog on every call (it changes once at
  // boot when models_catalog::init runs; cache invalidation is more
  // complexity than the few-entries-per-call saves).
  static std::mutex ids_mtx;
  static std::vector<std::string> ids;
  static bool built = false;
  std::lock_guard<std::mutex> lk(ids_mtx);
  if (!built) {
    for (const auto &row : models_catalog::local_piper_voices())
      ids.push_back(row.voice_id);
    built = true;
  }
  return ids;
}

std::string voice_language(const std::string &voice_id) {
  for (const auto &row : models_catalog::local_piper_voices()) {
    if (row.voice_id == voice_id)
      return row.language;
  }
  return {};
}

std::string default_voice_for(VoiceRole role) {
  // Catalog is ordered Atis, Tower, Ground, Center for the first four
  // required English voices; that ordering is the role mapping.
  const auto &rows = models_catalog::local_piper_voices();
  if (rows.empty())
    return {};
  auto pick = [&rows](size_t idx) {
    return rows[idx < rows.size() ? idx : 0].voice_id;
  };
  switch (role) {
  case VoiceRole::Atis:
    return pick(0);
  case VoiceRole::Tower:
    return pick(1);
  case VoiceRole::Ground:
    return pick(2);
  case VoiceRole::Center:
    return pick(3);
  }
  return rows[0].voice_id;
}

std::string default_voice_for(VoiceRole role, const std::string &language) {
  if (language == "de") {
    for (const auto &row : models_catalog::local_piper_voices()) {
      if (row.language == "de")
        return row.voice_id;
    }
  }
  return default_voice_for(role);
}

std::string sha256_file(const std::string &path) {
  std::FILE *f = std::fopen(path.c_str(), "rb");
  if (!f)
    return {};

  CC_SHA256_CTX ctx;
  CC_SHA256_Init(&ctx);

  // 1 MB chunks: large enough to amortise read syscalls, small enough
  // to avoid a single big allocation that competes with model load.
  // **Heap-allocated**: macOS pthreads default to a 512 KB stack, so a
  // 1 MB std::array<> on the stack would SIGSEGV the moment this
  // function is called from a worker thread (downloader/loader).
  static constexpr size_t kChunkBytes = 1024ULL * 1024;
  std::vector<unsigned char> buf(kChunkBytes);
  size_t n = 0;
  while ((n = std::fread(buf.data(), 1, buf.size(), f)) > 0) {
    CC_SHA256_Update(&ctx, buf.data(), static_cast<CC_LONG>(n));
  }
  bool eof_clean = std::feof(f) != 0;
  std::fclose(f);
  if (!eof_clean)
    return {}; // read error

  unsigned char digest[CC_SHA256_DIGEST_LENGTH];
  CC_SHA256_Final(digest, &ctx);

  static const char hex[] = "0123456789abcdef";
  std::string out(static_cast<size_t>(2) * CC_SHA256_DIGEST_LENGTH, '\0');
  for (size_t i = 0; i < CC_SHA256_DIGEST_LENGTH; ++i) {
    out[(2 * i)] = hex[(digest[i] >> 4) & 0xF];
    out[(2 * i) + 1] = hex[digest[i] & 0xF];
  }
  return out;
}

bool size_matches(const Entry &e, const std::string &full_path) {
  struct stat st{};
  if (stat(full_path.c_str(), &st) != 0)
    return false;
  if (!S_ISREG(st.st_mode))
    return false;
  return static_cast<uint64_t>(st.st_size) == e.size_bytes;
}

} // namespace model_manifest
