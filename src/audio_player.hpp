#ifndef AUDIO_PLAYER_HPP
#define AUDIO_PLAYER_HPP

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

} // namespace audio_player

#endif // AUDIO_PLAYER_HPP
