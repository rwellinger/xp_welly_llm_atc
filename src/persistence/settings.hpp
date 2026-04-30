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

#ifndef SETTINGS_HPP
#define SETTINGS_HPP

#include "persistence/model_manifest.hpp"

#include <string>

namespace settings {

void init();
void stop();
void save();

// Data directory path (plugin-relative <plugin>/data)
std::string get_data_dir();

// Region-scoped data directory (e.g. <data>/regions/eu or <data>/regions/us)
std::string region_data_dir();

// Getters
std::string pilot_callsign();
int active_com();
float volume();
bool debug_logging();
std::string pattern_direction();
bool disable_default_atc();
bool skip_radio_power_check();
bool show_phraseology_hints();
float auto_correction_factor();
std::string flow_region(); // "EU" or "US"

// Setters
std::string pilot_callsign_raw();
void set_pilot_callsign_raw(const std::string &raw);
std::string to_icao_phonetic(const std::string &raw);
void set_volume(float v);
void set_debug_logging(bool v);
void set_active_com(int com);
void set_pattern_direction(const std::string &v);
void set_disable_default_atc(bool v);
void set_skip_radio_power_check(bool v);
void set_show_phraseology_hints(bool v);
void set_auto_correction_factor(float v);
void set_flow_region(const std::string &v);

// Voice id (Piper voice_id, e.g. "en_US-lessac-medium") currently
// assigned to a logical ATC role. Defaults to the manifest default if
// the setting is missing or points at an unknown voice id.
std::string voice_for_role(model_manifest::VoiceRole role);
void set_voice_for_role(model_manifest::VoiceRole role,
                        const std::string &voice_id);

// Window geometry (-1 = use default/center)
float window_x();
float window_y();
float window_w();
float window_h();
void set_window_geometry(float x, float y, float w, float h);
void reset_window_geometry();

} // namespace settings

#endif // SETTINGS_HPP
