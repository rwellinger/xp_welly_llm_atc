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

// Single source of truth for the bundled manifest. SHA256 + size were
// captured during the milestone-05 spike against these exact URLs;
// regenerating means a user re-download, so don't change without
// thinking it through. The HuggingFace `resolve/main` URLs are stable
// pointers to the file's content-addressed blob — they will not move
// without a new revision.
const std::vector<Entry> &manifest() {
  static const std::vector<Entry> entries = {
      {Kind::WhisperModel, "ggml-small.en-q5_1.bin", 190098681ULL,
       "bfdff4894dcb76bbf647d56263ea2a96645423f1669176f4844a1bf8e478ad30",
       "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/"
       "ggml-small.en-q5_1.bin",
       "Whisper STT (small.en, q5_1)"},
      {Kind::LlamaModel, "Llama-3.2-3B-Instruct-Q4_K_M.gguf", 2019377696ULL,
       "6c1a2b41161032677be168d354123594c0e6e67d2b9227c84f296ad037c728ff",
       "https://huggingface.co/bartowski/Llama-3.2-3B-Instruct-GGUF/resolve/"
       "main/Llama-3.2-3B-Instruct-Q4_K_M.gguf",
       "Llama 3.2 3B Instruct (Q4_K_M)"},
      {Kind::PiperVoice, "en_US-lessac-medium.onnx", 63201294ULL,
       "5efe09e69902187827af646e1a6e9d269dee769f9877d17b16b1b46eeaaf019f",
       "https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/"
       "lessac/medium/en_US-lessac-medium.onnx",
       "Piper voice (en_US-lessac-medium)"},
      {Kind::PiperVoiceConfig, "en_US-lessac-medium.onnx.json", 4885ULL,
       "efe19c417bed055f2d69908248c6ba650fa135bc868b0e6abb3da181dab690a0",
       "https://huggingface.co/rhasspy/piper-voices/resolve/main/en/en_US/"
       "lessac/medium/en_US-lessac-medium.onnx.json",
       "Piper voice config"},
  };
  return entries;
}

} // namespace

const std::vector<Entry> &all() { return manifest(); }

const Entry &get(Kind kind) {
  for (const auto &e : manifest()) {
    if (e.kind == kind)
      return e;
  }
  std::abort(); // unreachable for valid Kind values
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
  // function is called from a worker thread (downloader/loader). std::
  // vector puts the storage on the heap; the std::array variant we
  // had before crashed X-Plane mid-SHA256 with no log.
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
