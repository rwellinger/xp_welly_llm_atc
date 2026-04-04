#include <XPLMDisplay.h>
#include <XPLMMenus.h>
#include <XPLMPlugin.h>
#include <XPLMProcessing.h>
#include <XPLMUtilities.h>

#include "atc_session.hpp"
#include "atc_ui.hpp"
#include "audio_recorder.hpp"
#include "ptt_input.hpp"
#include "settings.hpp"
#include "xplane_context.hpp"

static XPLMMenuID menu_id = nullptr;
static int menu_container_idx = -1;

static void menu_handler(void *, void *) { atc_ui::toggle(); }

static float flight_loop_cb(float, float, int, void *) {
  xplane_context::update();
  return -1.0f; // called every frame
}

PLUGIN_API int XPluginStart(char *name, char *sig, char *desc) {
  std::snprintf(name, 256, "Welly's ATC");
  std::snprintf(sig, 256, "ch.thWelly.wellys_atc");
  std::snprintf(desc, 256, "AI-powered ATC voice communication for VFR");

  XPLMDebugString("[xp_wellys_atc] Plugin started\n");

  settings::init();
  xplane_context::init();
  audio_recorder::init();
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

  return 1;
}

PLUGIN_API void XPluginStop() {
  if (menu_id) {
    XPLMDestroyMenu(menu_id);
    menu_id = nullptr;
  }

  atc_ui::stop();
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
