#ifndef SETTINGS_HPP
#define SETTINGS_HPP

#include <string>

namespace settings {

void init();
void stop();
void save();

// Keychain operations
bool save_api_key(const std::string &key);
std::string load_api_key();
void delete_api_key();
std::string get_api_key();

// Getters
bool api_key_saved();
int ptt_key_vk();
int ptt_joystick_button();
std::string tts_voice();
std::string tts_model();
std::string whisper_model();
std::string gpt_model();
bool gpt_fallback_enabled();
std::string pilot_callsign();
int active_com();
float volume();
bool debug_logging();
std::string audio_output_device();

// Setters
void set_tts_voice(const std::string &v);
void set_pilot_callsign(const std::string &cs);
void set_volume(float v);
void set_gpt_fallback_enabled(bool v);
void set_debug_logging(bool v);
void set_active_com(int com);
void set_audio_output_device(const std::string &uid);

} // namespace settings

#endif // SETTINGS_HPP
