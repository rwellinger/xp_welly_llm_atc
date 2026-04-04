#include "atc_ui.hpp"
#include "atc_session.hpp"
#include "settings.hpp"
#include "xplane_context.hpp"

#include <XPLMDisplay.h>

#include <imgui.h>
#include <imgui_impl_opengl2.h>

#if defined(__APPLE__)
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include <cstring>

namespace atc_ui {

static bool visible = false;
static XPLMWindowID window_id = nullptr;

// ImGui persistent buffers
static char api_key_buf[256] = {};
static char callsign_buf[256] = {};
static float save_feedback_timer = 0.0f;
static bool key_just_saved = false;
static bool buffers_initialized = false;

static const char *voice_names[] = {"alloy", "echo", "fable",
                                    "onyx",  "nova", "shimmer"};
static const int voice_count = 6;
static int voice_selection = 3; // default: onyx

static void draw_status_tab() {
  // PTT State
  auto ptt = atc_session::ptt_state();
  std::string label = atc_session::ptt_state_label();
  if (ptt == atc_session::PTTState::RECORDING) {
    ImGui::TextColored(ImVec4(1.0f, 0.2f, 0.2f, 1.0f), "%s", label.c_str());
  } else if (ptt == atc_session::PTTState::PROCESSING) {
    ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "%s", label.c_str());
  } else if (ptt == atc_session::PTTState::PLAYING) {
    ImGui::TextColored(ImVec4(0.2f, 0.8f, 1.0f, 1.0f), "%s", label.c_str());
  } else {
    ImGui::Text("%s", label.c_str());
  }

  // Last recording info
  float dur = atc_session::last_recording_duration();
  size_t samples = atc_session::last_recording_samples();
  size_t wav_bytes = atc_session::last_wav_bytes();
  if (samples > 0) {
    ImGui::Text("Last recording: %.2f s, %zu samples", dur, samples);
    ImGui::Text("WAV buffer: %zu bytes", wav_bytes);
  }

  ImGui::Separator();

  const auto &ctx = xplane_context::get();

  ImGui::Text("Airport: %s %s",
              ctx.nearest_airport_id.empty() ? "---"
                                             : ctx.nearest_airport_id.c_str(),
              ctx.nearest_airport_id.empty()
                  ? ""
                  : (ctx.is_towered_airport ? "(Towered)" : "(Uncontrolled)"));

  float active_freq =
      (ctx.active_com == 1) ? ctx.com1_freq_mhz : ctx.com2_freq_mhz;
  ImGui::Text("COM%d: %.3f MHz", ctx.active_com, active_freq);

  ImGui::Separator();
  ImGui::Text("Position: %.4f, %.4f", ctx.latitude, ctx.longitude);
  ImGui::Text("Altitude: %.0f ft MSL", ctx.altitude_ft_msl);
  ImGui::Text("GS: %.0f kts   IAS: %.0f kts", ctx.groundspeed_kts,
              ctx.indicated_airspeed_kts);
  ImGui::Text("VS: %.0f fpm   HDG: %.0f", ctx.vertical_speed_fpm,
              ctx.heading_true);
  ImGui::Text("On Ground: %s", ctx.on_ground ? "Yes" : "No");
  ImGui::Text("Engines: %s", ctx.engines_running ? "Running" : "Off");
  ImGui::Text("Aircraft: %s",
              ctx.aircraft_icao.empty() ? "---" : ctx.aircraft_icao.c_str());
}

static void draw_settings_tab() {
  // One-time init of buffers from settings
  if (!buffers_initialized) {
    std::strncpy(callsign_buf, settings::pilot_callsign().c_str(),
                 sizeof(callsign_buf) - 1);
    std::string voice = settings::tts_voice();
    for (int i = 0; i < voice_count; ++i) {
      if (voice == voice_names[i]) {
        voice_selection = i;
        break;
      }
    }
    buffers_initialized = true;
  }

  // API Key
  ImGui::Text("OpenAI API Key:");
  ImGui::InputText("##apikey", api_key_buf, sizeof(api_key_buf),
                   ImGuiInputTextFlags_Password);
  ImGui::SameLine();
  if (ImGui::Button("Save Key")) {
    if (settings::save_api_key(api_key_buf)) {
      key_just_saved = true;
      save_feedback_timer = 2.0f;
      std::memset(api_key_buf, 0, sizeof(api_key_buf));
    }
  }
  ImGui::SameLine();
  if (ImGui::Button("Delete Key")) {
    settings::delete_api_key();
    std::memset(api_key_buf, 0, sizeof(api_key_buf));
  }
  if (key_just_saved && save_feedback_timer > 0.0f) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0, 1, 0, 1), "Saved \xe2\x9c\x93");
    save_feedback_timer -= ImGui::GetIO().DeltaTime;
    if (save_feedback_timer <= 0.0f)
      key_just_saved = false;
  } else if (settings::api_key_saved()) {
    ImGui::SameLine();
    ImGui::TextDisabled("(Key stored in Keychain)");
  }

  ImGui::Separator();

  // Pilot callsign
  ImGui::InputText("Pilot Callsign", callsign_buf, sizeof(callsign_buf));

  // Volume
  float vol = settings::volume();
  if (ImGui::SliderFloat("Volume", &vol, 0.0f, 1.0f)) {
    settings::set_volume(vol);
  }

  // TTS Voice
  if (ImGui::Combo("TTS Voice", &voice_selection, voice_names, voice_count)) {
    settings::set_tts_voice(voice_names[voice_selection]);
  }

  // GPT Fallback
  bool gpt_fb = settings::gpt_fallback_enabled();
  if (ImGui::Checkbox("GPT Fallback", &gpt_fb)) {
    settings::set_gpt_fallback_enabled(gpt_fb);
  }

  // Debug logging
  bool debug = settings::debug_logging();
  if (ImGui::Checkbox("Debug Logging", &debug)) {
    settings::set_debug_logging(debug);
  }

  ImGui::Separator();
  int ptt_vk = settings::ptt_key_vk();
  if (ptt_vk < 0) {
    ImGui::TextDisabled(
        "PTT: not configured (set ptt_key_vk in settings.json)");
  } else {
    ImGui::Text("PTT key: VK code %d", ptt_vk);
  }

  ImGui::Separator();
  if (ImGui::Button("Save Settings")) {
    settings::set_pilot_callsign(callsign_buf);
    settings::save();
  }
}

static void draw_window(XPLMWindowID id, void *) {
  if (!visible)
    return;

  int left, top, right, bottom;
  XPLMGetWindowGeometry(id, &left, &top, &right, &bottom);

  ImGui_ImplOpenGL2_NewFrame();
  ImGui::NewFrame();

  ImGui::SetNextWindowPos(
      ImVec2(static_cast<float>(left), static_cast<float>(top)),
      ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(static_cast<float>(right - left),
                                  static_cast<float>(top - bottom)),
                           ImGuiCond_Always);

  ImGui::Begin("Welly's ATC", &visible,
               ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                   ImGuiWindowFlags_NoCollapse);

  if (ImGui::BeginTabBar("MainTabs")) {
    if (ImGui::BeginTabItem("Status")) {
      draw_status_tab();
      ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Settings")) {
      draw_settings_tab();
      ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
  }

  ImGui::End();

  ImGui::Render();
  ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());
}

static int handle_mouse(XPLMWindowID, int, int, XPLMMouseStatus, void *) {
  return 1;
}
static XPLMCursorStatus handle_cursor(XPLMWindowID, int, int, void *) {
  return xplm_CursorDefault;
}
static int handle_wheel(XPLMWindowID, int, int, int, int, void *) { return 1; }
static void handle_key(XPLMWindowID, char, XPLMKeyFlags, char, void *, int) {}

void init() {
  ImGui::CreateContext();
  ImGui_ImplOpenGL2_Init();

  XPLMCreateWindow_t params{};
  params.structSize = sizeof(params);
  params.left = 100;
  params.top = 550;
  params.right = 550;
  params.bottom = 100;
  params.visible = 0;
  params.drawWindowFunc = draw_window;
  params.handleMouseClickFunc = handle_mouse;
  params.handleCursorFunc = handle_cursor;
  params.handleMouseWheelFunc = handle_wheel;
  params.handleKeyFunc = handle_key;
  params.decorateAsFloatingWindow = xplm_WindowDecorationRoundRectangle;
  params.layer = xplm_WindowLayerFloatingWindows;

  window_id = XPLMCreateWindowEx(&params);
  XPLMSetWindowTitle(window_id, "Welly's ATC");
}

void stop() {
  if (window_id) {
    XPLMDestroyWindow(window_id);
    window_id = nullptr;
  }
  ImGui_ImplOpenGL2_Shutdown();
  ImGui::DestroyContext();

  buffers_initialized = false;
}

void toggle() {
  visible = !visible;
  if (window_id) {
    XPLMSetWindowIsVisible(window_id, visible ? 1 : 0);
  }
}

void draw() {
  if (visible && window_id) {
    draw_window(window_id, nullptr);
  }
}

} // namespace atc_ui
