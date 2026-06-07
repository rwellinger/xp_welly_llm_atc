/*
 * xp_wellys_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 */

#include "persistence/models_catalog.hpp"

#include "core/logging.hpp"

#include <fstream>
#include <json.hpp>
#include <mutex>

namespace models_catalog {

namespace {

using json = nlohmann::json;

struct Catalog {
  std::vector<Option> openai_stt, openai_lm, openai_tts, openai_voices;
  std::vector<Option> mistral_stt, mistral_lm, mistral_tts, mistral_voices;
  std::vector<LocalFileEntry> local_whisper;
  std::vector<LocalFileEntry> local_llama;
  std::vector<PiperVoiceEntry> local_piper;
  std::string piper_base =
      "https://huggingface.co/rhasspy/piper-voices/resolve/main";
};

std::mutex g_mtx;
Catalog g_cat;
bool g_loaded = false;

// ── Defaults ─────────────────────────────────────────────────────────
//
// The fallback values match `data/models_catalog.json` so a missing or
// malformed file degrades to the same UX. Kept in lock-step manually —
// CI does not enforce parity yet, but `tests/test_audit_logging.cpp`
// will surface any tag mismatch the moment the wire slugs diverge.
Option opt(std::string id, std::string label = {}) {
  Option o;
  o.id = std::move(id);
  o.label = label.empty() ? o.id : std::move(label);
  return o;
}

void ensure_defaults_locked() {
  g_cat = Catalog{}; // reset

  g_cat.openai_stt = {opt("whisper-1")};
  g_cat.openai_lm = {
      opt("gpt-4o-mini", "gpt-4o-mini (fast)"),
      opt("gpt-4o", "gpt-4o (best)"),
      opt("gpt-4.1-mini"),
      opt("gpt-4.1"),
  };
  g_cat.openai_tts = {
      opt("tts-1", "tts-1 (fast)"),
      opt("tts-1-hd", "tts-1-hd (HQ)"),
  };
  g_cat.openai_voices = {
      opt("alloy"), opt("echo"),
      opt("fable"), opt("onyx", "onyx (closest to ATC)"),
      opt("nova"),  opt("shimmer"),
  };

  g_cat.mistral_stt = {
      opt("voxtral-mini-transcribe-2507",
          "voxtral-mini-transcribe-2507 (recommended)"),
      opt("voxtral-mini-2507", "voxtral-mini-2507 (multimodal)"),
      opt("voxtral-small-2507", "voxtral-small-2507 (best, slower)"),
  };
  g_cat.mistral_lm = {
      opt("mistral-small-latest", "mistral-small-latest (fast)"),
      opt("mistral-medium-latest", "mistral-medium-latest"),
      opt("mistral-large-latest", "mistral-large-latest (recommended)"),
      opt("magistral-medium-latest", "magistral-medium-latest (reasoning)"),
  };
  g_cat.mistral_tts = {
      opt("voxtral-mini-tts-2603", "voxtral-mini-tts-2603 (only TTS model)"),
  };
  g_cat.mistral_voices = {
      opt("gb_oliver_neutral", "EN-GB Oliver (neutral) - closest to ICAO"),
      opt("gb_oliver_confident", "EN-GB Oliver (confident)"),
      opt("gb_oliver_cheerful", "EN-GB Oliver (cheerful)"),
      opt("gb_oliver_curious", "EN-GB Oliver (curious)"),
      opt("gb_oliver_excited", "EN-GB Oliver (excited)"),
      opt("gb_oliver_sad", "EN-GB Oliver (sad)"),
      opt("gb_oliver_angry", "EN-GB Oliver (angry)"),
      opt("en_paul_neutral", "EN-US Paul (neutral)"),
      opt("en_paul_confident", "EN-US Paul (confident)"),
      opt("en_paul_cheerful", "EN-US Paul (cheerful)"),
      opt("en_paul_happy", "EN-US Paul (happy)"),
      opt("en_paul_excited", "EN-US Paul (excited)"),
      opt("en_paul_frustrated", "EN-US Paul (frustrated)"),
      opt("en_paul_sad", "EN-US Paul (sad)"),
      opt("en_paul_angry", "EN-US Paul (angry)"),
      opt("gb_jane_neutral", "EN-GB Jane (neutral)"),
      opt("gb_jane_confident", "EN-GB Jane (confident)"),
      opt("gb_jane_curious", "EN-GB Jane (curious)"),
      opt("gb_jane_frustrated", "EN-GB Jane (frustrated)"),
      opt("gb_jane_jealousy", "EN-GB Jane (jealousy)"),
      opt("gb_jane_sad", "EN-GB Jane (sad)"),
      opt("gb_jane_shameful", "EN-GB Jane (shameful)"),
      opt("gb_jane_confused", "EN-GB Jane (confused)"),
      opt("gb_jane_sarcasm", "EN-GB Jane (sarcasm)"),
      opt("fr_marie_neutral", "FR Marie (neutral)"),
      opt("fr_marie_happy", "FR Marie (happy)"),
      opt("fr_marie_excited", "FR Marie (excited)"),
      opt("fr_marie_curious", "FR Marie (curious)"),
      opt("fr_marie_sad", "FR Marie (sad)"),
      opt("fr_marie_angry", "FR Marie (angry)"),
  };

  g_cat.local_whisper = {
      {"ggml-small.en-q5_1.bin", 190098681ULL,
       "bfdff4894dcb76bbf647d56263ea2a96645423f1669176f4844a1bf8e478ad30",
       "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/"
       "ggml-small.en-q5_1.bin",
       "Whisper STT (small.en, q5_1)", "en"},
      {"ggml-small-q5_1.bin", 190085487ULL,
       "ae85e4a935d7a567bd102fe55afc16bb595bdb618e11b2fc7591bc08120411bb",
       "https://huggingface.co/ggerganov/whisper.cpp/resolve/main/"
       "ggml-small-q5_1.bin",
       "Whisper STT (small multilingual, q5_1)", "de"},
  };
  g_cat.local_llama = {
      {"Llama-3.2-3B-Instruct-Q4_K_M.gguf", 2019377696ULL,
       "6c1a2b41161032677be168d354123594c0e6e67d2b9227c84f296ad037c728ff",
       "https://huggingface.co/bartowski/Llama-3.2-3B-Instruct-GGUF/resolve/"
       "main/Llama-3.2-3B-Instruct-Q4_K_M.gguf",
       "Llama 3.2 3B Instruct (Q4_K_M)", ""},
  };
  g_cat.local_piper = {
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
      {"de_DE-thorsten-medium", "de/de_DE/thorsten/medium", 63201294ULL,
       "7e64762d8e5118bb578f2eea6207e1a35a8e0c30595010b666f983fc87bb7819",
       4819ULL,
       "974adee790533adb273a1ac88f49027d2a1b8f0f2cf4905954a4791e79264e85",
       false, "de"},
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
  g_cat.piper_base = "https://huggingface.co/rhasspy/piper-voices/resolve/main";

  g_loaded = true;
}

// Pull an Option list out of `node[key]`. Missing/non-array sections
// leave the existing defaults untouched so a partial JSON only
// overrides what it specifies.
void parse_options(const json &node, const char *key,
                   std::vector<Option> &out) {
  if (!node.contains(key) || !node[key].is_array())
    return;
  std::vector<Option> tmp;
  for (const auto &el : node[key]) {
    if (!el.is_object())
      continue;
    Option o;
    o.id = el.value("id", std::string{});
    if (o.id.empty())
      continue;
    o.label = el.value("label", std::string{});
    if (o.label.empty())
      o.label = o.id;
    tmp.push_back(std::move(o));
  }
  if (!tmp.empty())
    out = std::move(tmp);
}

void parse_local_files(const json &node, const char *key,
                       std::vector<LocalFileEntry> &out) {
  if (!node.contains(key) || !node[key].is_array())
    return;
  std::vector<LocalFileEntry> tmp;
  for (const auto &el : node[key]) {
    if (!el.is_object())
      continue;
    LocalFileEntry e;
    e.filename = el.value("filename", std::string{});
    if (e.filename.empty())
      continue;
    e.size_bytes = el.value("size_bytes", 0ULL);
    e.sha256 = el.value("sha256", std::string{});
    e.url = el.value("url", std::string{});
    e.display_name = el.value("display_name", e.filename);
    e.language = el.value("language", std::string{});
    tmp.push_back(std::move(e));
  }
  if (!tmp.empty())
    out = std::move(tmp);
}

void parse_piper_voices(const json &node, std::vector<PiperVoiceEntry> &out) {
  if (!node.contains("piper_voices") || !node["piper_voices"].is_array())
    return;
  std::vector<PiperVoiceEntry> tmp;
  for (const auto &el : node["piper_voices"]) {
    if (!el.is_object())
      continue;
    PiperVoiceEntry v;
    v.voice_id = el.value("voice_id", std::string{});
    if (v.voice_id.empty())
      continue;
    v.url_subpath = el.value("url_subpath", std::string{});
    v.onnx_size = el.value("onnx_size", 0ULL);
    v.onnx_sha256 = el.value("onnx_sha256", std::string{});
    v.json_size = el.value("json_size", 0ULL);
    v.json_sha256 = el.value("json_sha256", std::string{});
    v.optional = el.value("optional", false);
    v.language = el.value("language", std::string{});
    tmp.push_back(std::move(v));
  }
  if (!tmp.empty())
    out = std::move(tmp);
}

} // namespace

bool init(const std::string &data_dir) {
  std::lock_guard<std::mutex> lk(g_mtx);
  ensure_defaults_locked();

  const std::string path = data_dir + "/models_catalog.json";
  std::ifstream f(path);
  if (!f.is_open()) {
    logging::info("models_catalog: %s not found, using built-in defaults",
                  path.c_str());
    return false;
  }

  try {
    json root;
    f >> root;

    if (root.contains("openai") && root["openai"].is_object()) {
      const auto &n = root["openai"];
      parse_options(n, "stt", g_cat.openai_stt);
      parse_options(n, "lm", g_cat.openai_lm);
      parse_options(n, "tts", g_cat.openai_tts);
      parse_options(n, "voices", g_cat.openai_voices);
    }
    if (root.contains("mistral") && root["mistral"].is_object()) {
      const auto &n = root["mistral"];
      parse_options(n, "stt", g_cat.mistral_stt);
      parse_options(n, "lm", g_cat.mistral_lm);
      parse_options(n, "tts", g_cat.mistral_tts);
      parse_options(n, "voices", g_cat.mistral_voices);
    }
    if (root.contains("local") && root["local"].is_object()) {
      const auto &n = root["local"];
      parse_local_files(n, "whisper", g_cat.local_whisper);
      parse_local_files(n, "llama", g_cat.local_llama);
      parse_piper_voices(n, g_cat.local_piper);
      if (n.contains("_piper_base") && n["_piper_base"].is_string())
        g_cat.piper_base = n["_piper_base"].get<std::string>();
    }
  } catch (const std::exception &e) {
    logging::error("models_catalog: parse error in %s: %s — using built-in "
                   "defaults",
                   path.c_str(), e.what());
    ensure_defaults_locked();
    return false;
  }

  logging::info("models_catalog: loaded %s (mistral_lm=%zu, mistral_stt=%zu, "
                "piper_voices=%zu)",
                path.c_str(), g_cat.mistral_lm.size(), g_cat.mistral_stt.size(),
                g_cat.local_piper.size());
  return true;
}

const std::vector<Option> &openai_stt_options() {
  std::lock_guard<std::mutex> lk(g_mtx);
  if (!g_loaded)
    ensure_defaults_locked();
  return g_cat.openai_stt;
}
const std::vector<Option> &openai_lm_options() {
  std::lock_guard<std::mutex> lk(g_mtx);
  if (!g_loaded)
    ensure_defaults_locked();
  return g_cat.openai_lm;
}
const std::vector<Option> &openai_tts_options() {
  std::lock_guard<std::mutex> lk(g_mtx);
  if (!g_loaded)
    ensure_defaults_locked();
  return g_cat.openai_tts;
}
const std::vector<Option> &openai_voice_options() {
  std::lock_guard<std::mutex> lk(g_mtx);
  if (!g_loaded)
    ensure_defaults_locked();
  return g_cat.openai_voices;
}
const std::vector<Option> &mistral_stt_options() {
  std::lock_guard<std::mutex> lk(g_mtx);
  if (!g_loaded)
    ensure_defaults_locked();
  return g_cat.mistral_stt;
}
const std::vector<Option> &mistral_lm_options() {
  std::lock_guard<std::mutex> lk(g_mtx);
  if (!g_loaded)
    ensure_defaults_locked();
  return g_cat.mistral_lm;
}
const std::vector<Option> &mistral_tts_options() {
  std::lock_guard<std::mutex> lk(g_mtx);
  if (!g_loaded)
    ensure_defaults_locked();
  return g_cat.mistral_tts;
}
const std::vector<Option> &mistral_voice_options() {
  std::lock_guard<std::mutex> lk(g_mtx);
  if (!g_loaded)
    ensure_defaults_locked();
  return g_cat.mistral_voices;
}
const std::vector<LocalFileEntry> &local_whisper_entries() {
  std::lock_guard<std::mutex> lk(g_mtx);
  if (!g_loaded)
    ensure_defaults_locked();
  return g_cat.local_whisper;
}
const std::vector<LocalFileEntry> &local_llama_entries() {
  std::lock_guard<std::mutex> lk(g_mtx);
  if (!g_loaded)
    ensure_defaults_locked();
  return g_cat.local_llama;
}
const std::vector<PiperVoiceEntry> &local_piper_voices() {
  std::lock_guard<std::mutex> lk(g_mtx);
  if (!g_loaded)
    ensure_defaults_locked();
  return g_cat.local_piper;
}
const std::string &piper_base_url() {
  std::lock_guard<std::mutex> lk(g_mtx);
  if (!g_loaded)
    ensure_defaults_locked();
  return g_cat.piper_base;
}

} // namespace models_catalog
