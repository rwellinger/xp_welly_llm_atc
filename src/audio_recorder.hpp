#ifndef AUDIO_RECORDER_HPP
#define AUDIO_RECORDER_HPP

#include <cstddef>
#include <cstdint>
#include <vector>

namespace audio_recorder {

void init();
void stop();

void start_recording();
void stop_recording();

std::vector<uint8_t> encode_wav();
float duration_seconds();
size_t buffer_samples();

} // namespace audio_recorder

#endif // AUDIO_RECORDER_HPP
