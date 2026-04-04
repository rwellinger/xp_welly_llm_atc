#ifndef ATC_SESSION_HPP
#define ATC_SESSION_HPP

#include <cstddef>
#include <string>

namespace atc_session {

enum class PTTState { IDLE, RECORDING, PROCESSING, PLAYING };

void init();
void stop();

void on_ptt_pressed();
void on_ptt_released();

PTTState ptt_state();
std::string ptt_state_label();

// Last recording info (populated after stop_recording)
float last_recording_duration();
size_t last_recording_samples();
size_t last_wav_bytes();

} // namespace atc_session

#endif // ATC_SESSION_HPP
