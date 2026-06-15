/*
 * xp_wellys_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <https://www.gnu.org/licenses/>.
 */

#include "atc/atc_templates.hpp"
#include "core/logging.hpp"
#include "persistence/settings.hpp"

#include <json.hpp>

#include <fstream>

namespace atc_templates {

static nlohmann::json templates_;
static nlohmann::json prompts_;
static bool loaded_ = false;
static bool prompts_loaded_ = false;

// Deep-merge overlay into base: objects are merged recursively (base keys
// win on conflict), arrays are appended, primitive values are ignored when
// the key already exists in base.  Used to layer ifr/ on top of vfr/.
static void merge_into(nlohmann::json &base, const nlohmann::json &overlay) {
  if (!overlay.is_object())
    return;
  for (auto &[key, val] : overlay.items()) {
    if (key.rfind("_comment", 0) == 0)
      continue; // skip comment-only keys
    if (!base.contains(key)) {
      base[key] = val;
    } else if (base[key].is_object() && val.is_object()) {
      merge_into(base[key], val);
    } else if (base[key].is_array() && val.is_array()) {
      for (auto &item : val)
        base[key].push_back(item);
    }
    // primitive conflict: base (vfr) wins; overlay (ifr) entry is ignored
  }
}

static bool try_load_file(const std::string &path, nlohmann::json &out) {
  std::ifstream in(path);
  if (!in.good())
    return false;
  try {
    in >> out;
    return true;
  } catch (...) {
    return false;
  }
}

static void load_from_file() {
  std::string base = settings::atc_profile_data_dir();

  // Try split layout (vfr/ + ifr/) first, then fall back to the legacy
  // flat file so that DE/US profiles (no vfr/ subdir) continue to work.
  nlohmann::json vfr;
  bool has_vfr = try_load_file(base + "/vfr/atc_templates.json", vfr);
  if (!has_vfr)
    has_vfr = try_load_file(base + "/atc_templates.json", vfr);

  if (!has_vfr) {
    logging::info("Warning: atc_templates.json not found");
    loaded_ = false;
    return;
  }

  templates_ = vfr;

  nlohmann::json ifr;
  if (try_load_file(base + "/ifr/atc_templates.json", ifr)) {
    merge_into(templates_, ifr);
    logging::info("ATC templates loaded (VFR + IFR merged)");
  } else {
    logging::info("ATC templates loaded (VFR only)");
  }

  loaded_ = true;
}

static void load_prompts() {
  std::string path = settings::get_data_dir() + "/atc_prompt_templates.json";
  std::ifstream in(path);
  if (!in.good()) {
    logging::info("Warning: atc_prompt_templates.json not found");
    prompts_loaded_ = false;
    return;
  }

  try {
    in >> prompts_;
    prompts_loaded_ = true;
    logging::info("ATC prompt templates loaded");
  } catch (...) {
    logging::info("Warning: failed to parse atc_prompt_templates.json");
    prompts_loaded_ = false;
  }
}

void init() {
  load_from_file();
  load_prompts();
}

void stop() {
  templates_.clear();
  prompts_.clear();
  loaded_ = false;
  prompts_loaded_ = false;
}

void reload() {
  load_from_file();
  load_prompts();
}

TemplateEntry lookup(bool is_towered, const std::string &state,
                     const std::string &intent_key, bool tower_only) {
  static const TemplateEntry fallback{"Say again, {callsign}.", "IDLE", false};

  if (!loaded_)
    return fallback;

  const char *type = is_towered ? "towered" : "uncontrolled";

  if (!templates_.contains(type) || !templates_[type].contains(state))
    return fallback;

  auto &state_obj = templates_[type][state];

  // Prefer tower_only variant when applicable, then exact intent, then _INVALID
  std::string tower_only_key = intent_key + "_TOWER_ONLY";
  const std::string *key = &intent_key;
  static const std::string invalid_key = "_INVALID";
  if (tower_only && state_obj.contains(tower_only_key))
    key = &tower_only_key;
  else if (!state_obj.contains(intent_key))
    key = &invalid_key;

  if (!state_obj.contains(*key))
    return fallback;

  auto &entry = state_obj[*key];
  return TemplateEntry{
      entry.value("response", fallback.response_template),
      entry.value("next_state", state),
      entry.value("requires_readback", false),
  };
}

std::vector<std::string> valid_intents(bool is_towered,
                                       const std::string &state) {
  std::vector<std::string> result;

  if (!loaded_)
    return result;

  const char *type = is_towered ? "towered" : "uncontrolled";

  if (!templates_.contains(type) || !templates_[type].contains(state))
    return result;

  for (auto &[key, _] : templates_[type][state].items()) {
    if (key != "_INVALID")
      result.push_back(key);
  }
  return result;
}

std::string fill(const std::string &tmpl,
                 const std::map<std::string, std::string> &vars) {
  std::string result = tmpl;
  for (auto &[key, value] : vars) {
    std::string placeholder = "{" + key + "}";
    std::string::size_type pos = 0;
    while ((pos = result.find(placeholder, pos)) != std::string::npos) {
      result.replace(pos, placeholder.size(), value);
      pos += value.size();
    }
  }
  return result;
}

std::string get_prompt(const std::string &key) {
  if (!prompts_loaded_ || !prompts_.contains(key))
    return {};
  auto &entry = prompts_[key];
  if (entry.is_object() && entry.contains("prompt"))
    return entry["prompt"].get<std::string>();
  if (entry.is_string())
    return entry.get<std::string>();
  return {};
}

std::string lookup_fallback(const std::string &key,
                            const std::string &default_value) {
  if (!loaded_ || !templates_.contains("fallbacks"))
    return default_value;
  auto &fb = templates_["fallbacks"];
  if (!fb.contains(key) || !fb[key].is_string())
    return default_value;
  return fb[key].get<std::string>();
}

std::string lookup_bzf_strict(const std::string &key,
                              const std::string &default_value) {
  if (!loaded_ || !templates_.contains("bzf_strict"))
    return default_value;
  auto &bs = templates_["bzf_strict"];
  if (!bs.contains(key) || !bs[key].is_string())
    return default_value;
  return bs[key].get<std::string>();
}

} // namespace atc_templates
