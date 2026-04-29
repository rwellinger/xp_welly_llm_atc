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

#ifndef GPT_CLIENT_HPP
#define GPT_CLIENT_HPP

#include "core/xplane_context.hpp"

#include <functional>
#include <string>

namespace gpt_client {

void init();
void stop();

void ask_async(
    const std::string &pilot_text, const xplane_context::XPlaneContext &ctx,
    std::function<void(std::string response, bool success)> callback);

void classify_intent_async(
    const std::string &transcript, const std::string &system_prompt,
    std::function<void(std::string intent_key, bool success)> callback);

void drain_callback_queue();

} // namespace gpt_client

#endif // GPT_CLIENT_HPP
