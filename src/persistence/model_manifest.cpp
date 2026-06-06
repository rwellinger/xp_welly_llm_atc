/*
 * xp_wellys_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 */

#include "persistence/model_manifest.hpp"

#include <CommonCrypto/CommonDigest.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstdlib>
#include <vector>

namespace model_manifest {

namespace {

// ── Voice catalog ─────────────────────────────────────────────────
//
// Hashes/sizes were captured against rhasspy/piper-voices on
// HuggingFace (see scripts/fetch_voice_hashes.sh). Order:
// 1. Required voices first, in role order (Atis, Tower, Ground, Center).
// 2. Optional alternatives last.
struct VoiceCatalogRow {
  std::string voice_id;
  std::string url_subpath; // relative to base
  uint64_t onnx_size;
  std::string onnx_sha256;
  uint64_t json_size;
  std::string json_sha256;
  bool optional;
  std::string language; // "en", "de" — drives loader/UI region filtering
};

const std::vector<VoiceCatalogRow> &voice_catalog() {
  static const std::vector<VoiceCatalogRow> rows = {
      // English defaults (in role order: Atis, Tower, Ground, Center)
      {"en_US-lessac-medium", "en/en_US/lessac/medium", 63201294ULL,
       "5efe09e69902187827af646e1a6e9d269dee769f9877d17b16b1b46eeaaf019f",
       4885ULL,
       "efe19c417bed055f2d69908248c6ba650fa135bc868b0e6abb3da181dab690a0",
       false, "en"},
      {"en_US-ryan-high", "en/en_US/ryan/high", 120786792ULL,
       "b3990d7606e183ec8dbfba70a4607074f162de1a0c412e0180d1ff60bb154eca",
       4166ULL,
       "c6d3b98f08315cb4bebf0d49d50fc4ff491b503c64b940cd3d5ca28543b48011",
       false, "en"},
      {"en_US-amy-medium", "en/en_US/amy/medium", 63201294ULL,
       "b3a6e47b57b8c7fbe6a0ce2518161a50f59a9cdd8a50835c02cb02bdd6206c18",
       4882ULL,
       "95a23eb4d42909d38df73bb9ac7f45f597dbfcde2d1bf9526fdeaf5466977d77",
       false, "en"},
      {"en_GB-alan-medium", "en/en_GB/alan/medium", 63201294ULL,
       "0a309668932205e762801f1efc2736cd4b0120329622adf62be09e56339d3330",
       4888ULL,
       "c0f0d124e5895c00e7c03b35dcc8287f319a6998a365b182deb5c8e752ee8c1e",
       false, "en"},
      // German default. Single voice covers all four roles. Hashes
      // captured 2026-06-04 against rhasspy/piper-voices @ main.
      {"de_DE-thorsten-medium", "de/de_DE/thorsten/medium", 63201294ULL,
       "7e64762d8e5118bb578f2eea6207e1a35a8e0c30595010b666f983fc87bb7819",
       4819ULL,
       "974adee790533adb273a1ac88f49027d2a1b8f0f2cf4905954a4791e79264e85",
       /*optional=*/false, "de"},
      // Optional English alternatives
      {"en_US-libritts_r-medium", "en/en_US/libritts_r/medium", 78580914ULL,
       "10bb85e071d616fcf4071f369f1799d0491492ab3c5d552ec19fb548fac13195",
       20123ULL,
       "b471dc60d2d8335e819c393d196d6fbf792817f40051257b269878505bc9afb3", true,
       "en"},
      {"en_US-hfc_female-medium", "en/en_US/hfc_female/medium", 63201294ULL,
       "914c473788fc1fa8b63ace1cdcdb44588f4ae523d3ab37df1536616835a140b7",
       5033ULL,
       "03f1fa0622b80463283592d97aca9f6e89aec345a5c56b7257723e0093c58b6c", true,
       "en"},
      {"en_US-norman-medium", "en/en_US/norman/medium", 63531379ULL,
       "b9739443232a80a59c7d18810dd856899bf16a7964725f5ab81ea49b1351cb71",
       4968ULL,
       "6c2db7f558a4a8deb9fe822583c1c5105f6c4e834dd0f9de8ad17a888ee9fe1d", true,
       "en"},
      {"en_GB-northern_english_male-medium",
       "en/en_GB/northern_english_male/medium", 63201294ULL,
       "57a219ae8e638873db7d18893304be5069c42868f392bb95c3ff17f0690d0689",
       4847ULL,
       "69557ed3d974463453e9b0c09dd99a7ed0e52b8b87b64b357dbeeb2540a97d47", true,
       "en"},
  };
  return rows;
}

constexpr const char *kPiperBase =
    "https://huggingface.co/rhasspy/piper-voices/resolve/main";

// Friendly display name for a Piper voice. Strips the locale prefix
// and turns underscores into spaces so the UI says "lessac (medium)"
// rather than "en_US-lessac-medium".
std::string voice_display_name(const std::string &voice_id) {
  std::string s = voice_id;
  // Drop the "en_XX-" prefix if present.
  size_t dash = s.find('-');
  if (dash != std::string::npos && dash <= 6)
    s = s.substr(dash + 1);
  return s;
}

const std::vector<Entry> &manifest() {
  static const std::vector<Entry> entries = []() {
    std::vector<Entry> v;
    // English-only Whisper: faster, more accurate for EN-only callers.
    v.push_back(
        {Kind::WhisperModel, "ggml-small.en-q5_1.bin", 190098681ULL,
         "bfdff4894dcb76bbf647d56263ea2a96645423f1669176f4844a1bf8e478ad30",
         "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/"
         "ggml-small.en-q5_1.bin",
         "Whisper STT (small.en, q5_1)",
         /*voice_id=*/"", /*optional=*/false, /*language=*/"en"});
    // Multilingual Whisper for German (and any future non-EN regions).
    // Hash captured 2026-06-04 against ggerganov/whisper.cpp @ main.
    v.push_back(
        {Kind::WhisperModel, "ggml-small-q5_1.bin", 190085487ULL,
         "ae85e4a935d7a567bd102fe55afc16bb595bdb618e11b2fc7591bc08120411bb",
         "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/"
         "ggml-small-q5_1.bin",
         "Whisper STT (small multilingual, q5_1)",
         /*voice_id=*/"", /*optional=*/false, /*language=*/"de"});
    v.push_back(
        {Kind::LlamaModel, "Llama-3.2-3B-Instruct-Q4_K_M.gguf", 2019377696ULL,
         "6c1a2b41161032677be168d354123594c0e6e67d2b9227c84f296ad037c728ff",
         "https://huggingface.co/bartowski/Llama-3.2-3B-Instruct-GGUF/"
         "resolve/main/Llama-3.2-3B-Instruct-Q4_K_M.gguf",
         "Llama 3.2 3B Instruct (Q4_K_M)",
         /*voice_id=*/"", /*optional=*/false, /*language=*/""});

    for (const auto &row : voice_catalog()) {
      const std::string onnx = row.voice_id + ".onnx";
      const std::string json = onnx + ".json";
      const std::string base =
          std::string(kPiperBase) + "/" + row.url_subpath + "/";
      const std::string disp =
          "Piper voice (" + voice_display_name(row.voice_id) + ")";
      v.push_back({Kind::PiperVoice, onnx, row.onnx_size, row.onnx_sha256,
                   base + onnx, disp, row.voice_id, row.optional,
                   row.language});
      v.push_back({Kind::PiperVoiceConfig, json, row.json_size, row.json_sha256,
                   base + json, disp + " — config", row.voice_id, row.optional,
                   row.language});
    }
    return v;
  }();
  return entries;
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
  static const std::vector<std::string> ids = []() {
    std::vector<std::string> v;
    for (const auto &row : voice_catalog())
      v.push_back(row.voice_id);
    return v;
  }();
  return ids;
}

std::string voice_language(const std::string &voice_id) {
  for (const auto &row : voice_catalog()) {
    if (row.voice_id == voice_id)
      return row.language;
  }
  return {};
}

std::string default_voice_for(VoiceRole role) {
  // The catalog is ordered Atis, Tower, Ground, Center for the first
  // four entries — that ordering is the role mapping.
  const auto &rows = voice_catalog();
  switch (role) {
  case VoiceRole::Atis:
    return rows[0].voice_id;
  case VoiceRole::Tower:
    return rows[1].voice_id;
  case VoiceRole::Ground:
    return rows[2].voice_id;
  case VoiceRole::Center:
    return rows[3].voice_id;
  }
  return rows[0].voice_id;
}

std::string default_voice_for(VoiceRole role, const std::string &language) {
  if (language == "de") {
    // We ship a single German voice; it covers every role.
    for (const auto &row : voice_catalog()) {
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
