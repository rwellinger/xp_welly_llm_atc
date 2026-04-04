#ifndef ATC_STATE_MACHINE_HPP
#define ATC_STATE_MACHINE_HPP

#include "intent_parser.hpp"
#include "xplane_context.hpp"

#include <string>

namespace atc_state_machine {

enum class ATCState {
  IDLE,
  GROUND_CONTACT,
  TAXI_CLEARED,
  TOWER_CONTACT,
  DEPARTURE_CLEARED,
  PATTERN_ENTRY,
  LANDING_CLEARED,
  UNICOM_ACTIVE,
};

struct ATCResponse {
  std::string text;
  ATCState next_state = ATCState::IDLE;
  bool requires_readback = false;
};

void init();
void stop();
void reset();

ATCState get_state();
const char *state_name(ATCState state);

ATCResponse process(const intent_parser::PilotMessage &msg,
                    const xplane_context::XPlaneContext &ctx);

} // namespace atc_state_machine

#endif // ATC_STATE_MACHINE_HPP
