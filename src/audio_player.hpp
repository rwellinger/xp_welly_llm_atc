#ifndef AUDIO_PLAYER_HPP
#define AUDIO_PLAYER_HPP

#include <cstdint>
#include <string>
#include <vector>

namespace audio_player {

struct AudioDevice {
  std::string uid;  // Core Audio UID (empty = system default)
  std::string name; // Human-readable name
};

void init();
void stop();

// Enumerate available output devices (first entry is always "System Default")
const std::vector<AudioDevice> &get_output_devices();

// Refresh device list (call if user plugs in new device)
void refresh_devices();

// Play a short PTT click sound on the selected device
void play_ptt_click();

// Play MP3 data through speakers at given volume (0.0–1.0)
void play(const std::vector<uint8_t> &mp3_data, float volume);

// Returns true while audio is being played back
bool is_playing();

} // namespace audio_player

#endif // AUDIO_PLAYER_HPP
