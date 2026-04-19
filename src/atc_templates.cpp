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

#include "atc_templates.hpp"
#include "logging.hpp"
#include "settings.hpp"

#include <json.hpp>

#include <fstream>

namespace atc_templates {

static nlohmann::json templates_;
static nlohmann::json prompts_;
static bool loaded_ = false;
static bool prompts_loaded_ = false;

static void load_from_file() {
  std::string path = settings::region_data_dir() + "/atc_templates.json";
  std::ifstream in(path);
  if (!in.good()) {
    logging::info("Warning: atc_templates.json not found");
    loaded_ = false;
    return;
  }

  try {
    in >> templates_;
    loaded_ = true;
    logging::info("ATC templates loaded");
  } catch (...) {
    logging::info("Warning: failed to parse atc_templates.json");
    loaded_ = false;
  }
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
                     const std::string &intent_key) {
  static const TemplateEntry fallback{"Say again, {callsign}.", "IDLE", false};

  if (!loaded_)
    return fallback;

  const char *type = is_towered ? "towered" : "uncontrolled";

  if (!templates_.contains(type) || !templates_[type].contains(state))
    return fallback;

  auto &state_obj = templates_[type][state];

  // Try exact intent key first, then _INVALID fallback
  const std::string *key = &intent_key;
  static const std::string invalid_key = "_INVALID";
  if (!state_obj.contains(intent_key))
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

} // namespace atc_templates
