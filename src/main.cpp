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

#include <XPLMDataAccess.h>
#include <XPLMDisplay.h>
#include <XPLMMenus.h>
#include <XPLMPlugin.h>
#include <XPLMProcessing.h>
#include <XPLMUtilities.h>

#include "airport_vrps.hpp"
#include "airspace_db.hpp"
#include "atc_session.hpp"
#include "atc_state_machine.hpp"
#include "atc_templates.hpp"
#include "atc_ui.hpp"
#include "atis_generator.hpp"
#include "audio_player.hpp"
#include "audio_recorder.hpp"
#include "flight_phase.hpp"
#include "logging.hpp"
#include "openai/gpt_client.hpp"
#include "openai/tts_client.hpp"
#include "openai/whisper_client.hpp"
#include "ptt_input.hpp"
#include "settings.hpp"
#include "xplane_context.hpp"

static XPLMMenuID menu_id = nullptr;
static int menu_container_idx = -1;
static XPLMCommandRef cmd_atc_panel_ = nullptr;

static void menu_handler(void *, void *item_ref) {
  intptr_t idx = reinterpret_cast<intptr_t>(item_ref);
  if (idx == 0)
    atc_ui::toggle();
  else if (idx == 1)
    atc_ui::toggle_atc_panel();
  else if (idx == 2)
    atc_ui::reset_window_position();
}

static int atc_panel_cmd_handler(XPLMCommandRef, XPLMCommandPhase phase,
                                 void *) {
  if (phase == xplm_CommandBegin)
    atc_ui::toggle_atc_panel();
  return 0;
}

static int atis_check_counter_ = 0;
static float last_elapsed_ = 0.0f;

static XPLMDataRef dr_atc_verbose_ = nullptr;
static XPLMDataRef dr_atc_show_hist_ = nullptr;

static float flight_loop_cb(float, float, int, void *) {
  float now = XPLMGetElapsedTime();
  float dt = (last_elapsed_ > 0.0f) ? (now - last_elapsed_) : (1.0f / 60.0f);
  last_elapsed_ = now;

  xplane_context::update();
  flight_phase::update(xplane_context::get(), dt);
  // Check ATIS for updates ~1/s (every 60 frames)
  if (++atis_check_counter_ % 60 == 0)
    atis_generator::check_for_update(xplane_context::get());
  ptt_input::update();
  whisper_client::drain_callback_queue();
  gpt_client::drain_callback_queue();
  tts_client::drain_callback_queue();
  atc_session::update();

  if (settings::disable_default_atc()) {
    if (dr_atc_verbose_ && XPLMGetDatai(dr_atc_verbose_) != 0)
      XPLMSetDatai(dr_atc_verbose_, 0);
    if (dr_atc_show_hist_ && XPLMGetDatai(dr_atc_show_hist_) != 0)
      XPLMSetDatai(dr_atc_show_hist_, 0);
  }

  return -1.0f; // called every frame
}

PLUGIN_API int XPluginStart(char *name, char *sig, char *desc) {
  // Required for X-Plane installs on external volumes — without it the SDK
  // returns HFS paths that lose the /Volumes/<name>/ mount prefix when
  // naively converted to POSIX, causing all file I/O to fail.
  XPLMEnableFeature("XPLM_USE_NATIVE_PATHS", 1);

#ifdef XP_WELLYS_ATC_VERSION
  std::snprintf(name, 256, "Welly's ATC v%s", XP_WELLYS_ATC_VERSION);
#else
  std::snprintf(name, 256, "Welly's ATC");
#endif
  std::snprintf(sig, 256, "ch.thWelly.wellys_atc");
  std::snprintf(desc, 256, "AI-powered ATC voice communication for VFR");

  logging::set_sink(&XPLMDebugString);
  logging::info("Plugin started");

  settings::init();
  atc_templates::init();
  airport_vrps::init();
  {
    char raw[2048] = {};
    XPLMGetSystemPath(raw);
    std::string sys(raw);
#if defined(__APPLE__)
    if (sys.find(':') != std::string::npos &&
        sys.find('/') == std::string::npos) {
      auto colon = sys.find(':');
      std::string posix = sys.substr(colon + 1);
      for (char &c : posix)
        if (c == ':')
          c = '/';
      sys = "/" + posix;
    }
#endif
    if (!sys.empty() && sys.back() != '/')
      sys += '/';
    airspace_db::init(sys + "Custom Data/1200 atc data/Earth nav data/atc.dat");
  }
  xplane_context::init();
  flight_phase::init();
  atis_generator::init();
  audio_recorder::init();
  audio_player::init();
  whisper_client::init();
  gpt_client::init();
  tts_client::init();
  atc_state_machine::init();
  atc_ui::init();

  // DataRefs for silencing X-Plane's default ATC
  dr_atc_verbose_ = XPLMFindDataRef("sim/atc/atc_verbose");
  dr_atc_show_hist_ = XPLMFindDataRef("sim/atc/atc_show_hist");

  // Flight loop
  XPLMCreateFlightLoop_t loop_params{};
  loop_params.structSize = sizeof(loop_params);
  loop_params.phase = xplm_FlightLoop_Phase_AfterFlightModel;
  loop_params.callbackFunc = flight_loop_cb;
  XPLMFlightLoopID loop_id = XPLMCreateFlightLoop(&loop_params);
  XPLMScheduleFlightLoop(loop_id, -1.0f, true);

  // ATC Panel command (bindable via X-Plane keyboard/joystick settings)
  cmd_atc_panel_ =
      XPLMCreateCommand("xp_wellys_atc/atc_panel", "Toggle ATC Commands Panel");
  XPLMRegisterCommandHandler(cmd_atc_panel_, atc_panel_cmd_handler, 1, nullptr);

  // Menu
  menu_container_idx =
      XPLMAppendMenuItem(XPLMFindPluginsMenu(), "Welly's ATC", nullptr, 0);
  menu_id = XPLMCreateMenu("Welly's ATC", XPLMFindPluginsMenu(),
                           menu_container_idx, menu_handler, nullptr);
  XPLMAppendMenuItem(menu_id, "Open / Close", nullptr, 0);
  // NOLINTBEGIN(performance-no-int-to-ptr)
  XPLMAppendMenuItem(menu_id, "ATC Commands",
                     reinterpret_cast<void *>(uintptr_t{1}), 0);
  XPLMAppendMenuItem(menu_id, "Reset Window Position",
                     reinterpret_cast<void *>(uintptr_t{2}), 0);
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
  atis_generator::stop();
  flight_phase::stop();
  xplane_context::stop();
  airspace_db::stop();
  airport_vrps::stop();
  atc_templates::stop();
  settings::stop();

  logging::info("Plugin stopped");
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
