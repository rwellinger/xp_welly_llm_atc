#include "ptt_input.hpp"
#include "atc_session.hpp"

#include <XPLMUtilities.h>

namespace ptt_input {

static XPLMCommandRef ptt_cmd_ = nullptr;

static int ptt_command_handler(XPLMCommandRef /*cmd*/, XPLMCommandPhase phase,
                               void * /*refcon*/) {
  if (phase == xplm_CommandBegin) {
    atc_session::on_ptt_pressed();
  } else if (phase == xplm_CommandEnd) {
    atc_session::on_ptt_released();
  }
  return 0; // 0 = we handle it, don't pass to other handlers
}

void init() {
  ptt_cmd_ =
      XPLMCreateCommand("xp_wellys_atc/ptt", "Welly's ATC: Push-to-Talk");
  XPLMRegisterCommandHandler(ptt_cmd_, ptt_command_handler, 1 /* before */,
                             nullptr);
  XPLMDebugString(
      "[xp_wellys_atc] PTT command registered: xp_wellys_atc/ptt\n");
}

void stop() {
  if (ptt_cmd_) {
    XPLMUnregisterCommandHandler(ptt_cmd_, ptt_command_handler, 1, nullptr);
    ptt_cmd_ = nullptr;
  }
}

} // namespace ptt_input
