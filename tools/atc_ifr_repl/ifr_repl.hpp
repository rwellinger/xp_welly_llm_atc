/*
 * xp_wellys_atc - headless IFR test CLI
 *
 * Interactive REPL for simulating IFR approach flows end-to-end
 * without X-Plane. Supports poll_approach, route_tracker, fly, goto,
 * training_jump_*, and standard process_transcript (say).
 */

#ifndef IFR_REPL_HPP
#define IFR_REPL_HPP

#include "core/xplane_context.hpp"

#include <string>

namespace ifr_repl {

int run(xplane_context::XPlaneContext ctx, std::string pilot_callsign);

} // namespace ifr_repl

#endif
