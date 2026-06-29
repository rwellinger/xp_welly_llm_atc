/*
 * xp_wellys_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 */

#ifndef PERSISTENCE_MODEL_MANIFEST_HPP
#define PERSISTENCE_MODEL_MANIFEST_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace model_manifest {

enum class Kind {
  WhisperModel,
  LlamaModel,
  PiperVoice,       // .onnx
  PiperVoiceConfig, // .onnx.json
};

// Pinned identity of a single model file the plugin needs at runtime.
// The bundled values were captured during the milestone-05 spike on
// the same source URLs the user will hit; treat them as authoritative
// and do not regenerate without updating the README's manual-fallback
// table.
//
// For Piper voices the manifest contains *both* the .onnx and the
// .onnx.json entry, paired by `voice_id`. The downloader treats them
// as independent files; the loader pairs them up by voice_id when
// instantiating Piper synthesizers.
struct Entry {
  Kind kind;
  std::string filename;     // basename only — lives in models_dir()
  uint64_t size_bytes;      // exact expected size, used as a fast pre-check
  std::string sha256_hex;   // lowercase 64-char hex, post-download verify
  std::string url;          // direct HTTPS GET; HuggingFace is the only source
  std::string display_name; // for the UI status panel

  // Piper-only fields. Empty/false for Whisper/Llama entries.
  // `voice_id` is the basename without the file extension (e.g.
  // "en_US-lessac-medium") and is shared between the .onnx and
  // .onnx.json entries that belong together.
  std::string voice_id;
  bool optional = false; // optional voices are not auto-downloaded

  // ISO-639-1 ("en") for language-specific entries (Whisper, Piper
  // voices). Empty for language-agnostic entries like Llama. The loader
  // picks Whisper + voices by matching this against
  // settings::backend_language(); the UI filters/badges by it.
  std::string language;
};

// A logical ATC role that a voice can be assigned to. Each role has
// exactly one *default* voice in the manifest; users can re-assign at
// runtime via settings.
enum class VoiceRole {
  Atis,
  Tower,
  Ground,
  Center,
};

const std::vector<VoiceRole> &all_roles();
const char *role_name(VoiceRole role);
bool role_from_name(const std::string &name, VoiceRole &out);

// Stable string key used as the unique identifier across loader /
// downloader / UI maps. For Whisper/Llama it collapses onto a constant;
// for Piper it folds in voice_id + onnx-vs-json discriminator. Two
// entries with the same key are the same file.
std::string entry_key(const Entry &e);

// All entries the plugin knows about. Order is the loading order and
// is also what the UI iterates for its rows. Required entries come
// first (Whisper, Llama, the four default voices), optional voices
// last.
const std::vector<Entry> &all();

// Look up a Whisper/Llama entry. Aborts on Piper kinds — those need
// the voice-aware getter below. For Whisper, this returns the *first*
// matching entry irrespective of language; prefer get_for_language()
// when the language matters.
const Entry &get(Kind kind);

// Look up a Whisper/Llama entry by (kind, language). Prefers an exact
// language match; falls back to a language-agnostic entry (language=="")
// for kinds like Llama. Aborts if nothing matches.
const Entry &get_for_language(Kind kind, const std::string &language);

// Look up any entry by (kind, filename). Returns nullptr if no match.
// Used by the local_stt_model setting to select a specific Whisper file.
const Entry *find_by_filename(Kind kind, const std::string &filename);

// Look up a Piper voice entry by (kind, voice_id). Returns nullptr
// if no match — voice_id may be a typo or a manifest mismatch.
const Entry *get_voice(Kind kind, const std::string &voice_id);

// All distinct voice ids in manifest order. UI dropdowns iterate
// this; first the four required voices, then the optionals.
const std::vector<std::string> &voice_ids();

// Language of a voice id ("en"), or empty if the id is unknown.
std::string voice_language(const std::string &voice_id);

// Default voice id for a role — used to seed settings on first run.
std::string default_voice_for(VoiceRole role);

// Language-aware default voice id for a role. All profiles speak
// English, so this delegates to the per-role English defaults above.
std::string default_voice_for(VoiceRole role, const std::string &language);

// Compute SHA256 of `path`. Returns lowercase 64-char hex on success
// or an empty string if the file cannot be opened. Streams the file in
// chunks so files of any size work; safe to call from a worker thread.
std::string sha256_file(const std::string &path);

// Cheap check: does the file exist with the expected size?
// SHA256 verification is the slow path and should only run after this
// returns true.
bool size_matches(const Entry &e, const std::string &full_path);

} // namespace model_manifest

#endif // PERSISTENCE_MODEL_MANIFEST_HPP
