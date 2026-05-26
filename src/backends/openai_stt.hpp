/*
 * xp_wellys_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 */

#ifndef BACKENDS_OPENAI_STT_HPP
#define BACKENDS_OPENAI_STT_HPP

#include "backends/i_speech_to_text.hpp"
#include "backends/openai_common.hpp"

#include <string>

namespace backends {

// ISpeechToText backed by OpenAI's /v1/audio/transcriptions endpoint
// ("whisper-1"). Synchronous — backends::manager runs it on a worker
// thread. Every call emits a [STT-OPENAI] audit log line.
class OpenAiStt final : public ISpeechToText {
public:
  OpenAiStt(std::string api_key, std::string model,
            std::string base_url = openai_common::kDefaultBaseUrl);

  std::string transcribe(const std::vector<float> &pcm_16k_mono,
                         const std::string &airport_context) override;

private:
  std::string api_key_;
  std::string model_;
  std::string base_url_;
};

} // namespace backends

#endif // BACKENDS_OPENAI_STT_HPP
