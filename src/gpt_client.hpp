#ifndef GPT_CLIENT_HPP
#define GPT_CLIENT_HPP

#include "xplane_context.hpp"

#include <functional>
#include <string>

namespace gpt_client {

void init();
void stop();

void ask_async(const std::string &pilot_text,
               const xplane_context::XPlaneContext &ctx,
               std::function<void(std::string response, bool success)> callback);

void drain_callback_queue();

} // namespace gpt_client

#endif // GPT_CLIENT_HPP
