#include <XPLMDisplay.h>
#include <XPLMMenus.h>
#include <XPLMPlugin.h>
#include <XPLMProcessing.h>
#include <XPLMUtilities.h>

#include "atc_session.hpp"
#include "atc_state_machine.hpp"
#include "atc_ui.hpp"
#include "audio_player.hpp"
#include "audio_recorder.hpp"
#include "gpt_client.hpp"
#include "ptt_input.hpp"
#include "settings.hpp"
#include "tts_client.hpp"
#include "whisper_client.hpp"
#include "xplane_context.hpp"

static XPLMMenuID menu_id = nullptr;
static int menu_container_idx = -1;

static void menu_handler(void *, void *item_ref) {
  intptr_t idx = reinterpret_cast<intptr_t>(item_ref);
  if (idx == 0)
    atc_ui::toggle();
  else if (idx == 1)
    atc_ui::reset_window_position();
}

static float flight_loop_cb(float, float, int, void *) {
  xplane_context::update();
  ptt_input::update();
  whisper_client::drain_callback_queue();
  gpt_client::drain_callback_queue();
  tts_client::drain_callback_queue();
  atc_session::update();
  return -1.0f; // called every frame
}

PLUGIN_API int XPluginStart(char *name, char *sig, char *desc) {
#ifdef XP_WELLYS_ATC_VERSION
  std::snprintf(name, 256, "Welly's ATC v%s", XP_WELLYS_ATC_VERSION);
#else
  std::snprintf(name, 256, "Welly's ATC");
#endif
  std::snprintf(sig, 256, "ch.thWelly.wellys_atc");
  std::snprintf(desc, 256, "AI-powered ATC voice communication for VFR");

  XPLMDebugString("[xp_wellys_atc] Plugin started\n");

  settings::init();
  xplane_context::init();
  audio_recorder::init();
  audio_player::init();
  whisper_client::init();
  gpt_client::init();
  tts_client::init();
  atc_state_machine::init();
  atc_ui::init();

  // Flight loop
  XPLMCreateFlightLoop_t loop_params{};
  loop_params.structSize = sizeof(loop_params);
  loop_params.phase = xplm_FlightLoop_Phase_AfterFlightModel;
  loop_params.callbackFunc = flight_loop_cb;
  XPLMFlightLoopID loop_id = XPLMCreateFlightLoop(&loop_params);
  XPLMScheduleFlightLoop(loop_id, -1.0f, true);

  // Menu
  menu_container_idx =
      XPLMAppendMenuItem(XPLMFindPluginsMenu(), "Welly's ATC", nullptr, 0);
  menu_id = XPLMCreateMenu("Welly's ATC", XPLMFindPluginsMenu(),
                           menu_container_idx, menu_handler, nullptr);
  XPLMAppendMenuItem(menu_id, "Open / Close", nullptr, 0);
  // NOLINTBEGIN(performance-no-int-to-ptr)
  XPLMAppendMenuItem(menu_id, "Reset Window Position",
                     reinterpret_cast<void *>(uintptr_t{1}), 0);
  // NOLINTEND(performance-no-int-to-ptr)

  return 1;
}

PLUGIN_API void XPluginStop() {
  if (menu_id) {
    XPLMDestroyMenu(menu_id);
    menu_id = nullptr;
  }

  atc_ui::stop();
  atc_state_machine::stop();
  tts_client::stop();
  gpt_client::stop();
  whisper_client::stop();
  audio_player::stop();
  audio_recorder::stop();
  xplane_context::stop();
  settings::stop();

  XPLMDebugString("[xp_wellys_atc] Plugin stopped\n");
}

PLUGIN_API int XPluginEnable() {
  ptt_input::init();
  atc_session::init();
  return 1;
}

PLUGIN_API void XPluginDisable() {
  ptt_input::stop();
  atc_session::stop();
}

PLUGIN_API void XPluginReceiveMessage(XPLMPluginID, int, void *) {}
