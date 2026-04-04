#include "atc_ui.hpp"
#include "atc_session.hpp"
#include "atc_state_machine.hpp"
#include "audio_player.hpp"
#include "intent_parser.hpp"
#include "settings.hpp"
#include "xplane_context.hpp"

#include <XPLMDataAccess.h>
#include <XPLMDisplay.h>
#include <XPLMGraphics.h>
#include <XPLMUtilities.h>

#include <imgui.h>
#include <imgui_impl_opengl2.h>

#if defined(__APPLE__)
#include <OpenGL/gl.h>
#else
#include <GL/gl.h>
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace atc_ui {

// ── State ────────────────────────────────────────────────────────
// The XPLM window is full-screen and invisible (DecorationNone).
// It exists only to capture mouse/keyboard events and feed them to ImGui.
// ImGui draws its own window on top.
static XPLMWindowID window_id = nullptr;
static bool visible = false;

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

// ── Time ─────────────────────────────────────────────────────────
static double last_frame_time_ = 0.0;
static double get_xp_time() {
  static XPLMDataRef dr = nullptr;
  if (!dr)
    dr = XPLMFindDataRef("sim/time/total_running_time_sec");
  return dr ? static_cast<double>(XPLMGetDataf(dr)) : 0.0;
}

static size_t last_transcript_count_ = 0;

// ── Tab drawing ──────────────────────────────────────────────────

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

  // ATC State
  ImGui::SameLine();
  ImGui::Text("   ATC State: %s",
              atc_state_machine::state_name(atc_state_machine::get_state()));
  ImGui::SameLine();
  if (ImGui::SmallButton("Reset")) {
    atc_state_machine::reset();
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

  // Last Parsed Intent
  ImGui::Separator();
  ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "Last Parsed Intent");

  const auto &pm = atc_session::last_pilot_message();
  if (!pm.raw_transcript.empty()) {
    ImGui::Text("Intent: %s", intent_parser::intent_name(pm.intent));
    ImGui::Text("Confidence: %.2f", pm.confidence);
    if (!pm.callsign.empty())
      ImGui::Text("Callsign: %s", pm.callsign.c_str());
    if (!pm.runway.empty())
      ImGui::Text("Runway: %s", pm.runway.c_str());
    ImGui::TextWrapped("Transcript: %s", pm.raw_transcript.c_str());
  } else {
    ImGui::TextDisabled("(no transcript yet)");
  }
}

static void draw_transcript_tab() {
  if (ImGui::Button("Clear")) {
    atc_session::clear_transcript();
    last_transcript_count_ = 0;
  }

  ImGui::Separator();

  ImGui::BeginChild("TranscriptScroll", ImVec2(0, 0), false,
                     ImGuiWindowFlags_HorizontalScrollbar);

  const auto &entries = atc_session::transcript_entries();
  for (const auto &entry : entries) {
    int mins = static_cast<int>(entry.sim_time) / 60;
    int secs = static_cast<int>(entry.sim_time) % 60;
    char line[512];
    if (entry.is_pilot) {
      std::snprintf(line, sizeof(line), "[%02d:%02d] You: %s", mins, secs,
                    entry.text.c_str());
      ImGui::TextUnformatted(line);
    } else {
      const auto &cx = xplane_context::get();
      std::string prefix = cx.nearest_airport_id.empty()
                               ? "ATC"
                               : cx.nearest_airport_id + " ATC";
      std::snprintf(line, sizeof(line), "[%02d:%02d] %s: %s", mins, secs,
                    prefix.c_str(), entry.text.c_str());
      ImGui::TextColored(ImVec4(0.0f, 1.0f, 1.0f, 1.0f), "%s", line);
    }
  }

  // Auto-scroll on new entries
  if (entries.size() != last_transcript_count_) {
    ImGui::SetScrollHereY(1.0f);
    last_transcript_count_ = entries.size();
  }

  ImGui::EndChild();
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
  if (ImGui::Button("Paste")) {
    FILE *fp = popen("pbpaste", "r"); // NOLINT(cert-env33-c,bugprone-command-processor)
    if (fp) {
      char clip[256] = {};
      if (fgets(clip, sizeof(clip), fp)) {
        // Strip trailing newline
        size_t len = std::strlen(clip);
        if (len > 0 && clip[len - 1] == '\n')
          clip[len - 1] = '\0';
        std::strncpy(api_key_buf, clip, sizeof(api_key_buf) - 1);
      }
      pclose(fp);
    }
  }
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

  // Audio output device
  {
    const auto &devs = audio_player::get_output_devices();
    std::string current_uid = settings::audio_output_device();
    int current_idx = 0;
    for (int i = 0; i < static_cast<int>(devs.size()); ++i) {
      if (devs[i].uid == current_uid) {
        current_idx = i;
        break;
      }
    }
    if (ImGui::BeginCombo("Audio Output",
                          devs.empty() ? "---"
                                       : devs[current_idx].name.c_str())) {
      for (int i = 0; i < static_cast<int>(devs.size()); ++i) {
        bool selected = (i == current_idx);
        if (ImGui::Selectable(devs[i].name.c_str(), selected)) {
          settings::set_audio_output_device(devs[i].uid);
          settings::save();
        }
        if (selected)
          ImGui::SetItemDefaultFocus();
      }
      ImGui::EndCombo();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Refresh")) {
      audio_player::refresh_devices();
    }
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
  if (ImGui::Button("Save Settings")) {
    settings::set_pilot_callsign(callsign_buf);
    settings::save();
  }
}

// ── XPLM window callbacks (input capture only) ──────────────────

static void wnd_draw_cb(XPLMWindowID, void *) {
  // Nothing — rendering happens in the draw phase callback
}

static int wnd_mouse_cb(XPLMWindowID wnd, int x, int y,
                         XPLMMouseStatus status, void *) {
  int left, top, right, bottom;
  XPLMGetWindowGeometry(wnd, &left, &top, &right, &bottom);

  float mx = static_cast<float>(x - left);
  float my = static_cast<float>(top - y);

  ImGuiIO &io = ImGui::GetIO();
  io.AddMousePosEvent(mx, my);
  if (status == xplm_MouseDown)
    io.AddMouseButtonEvent(0, true);
  if (status == xplm_MouseUp)
    io.AddMouseButtonEvent(0, false);
  return 1;
}

static int wnd_rclick_cb(XPLMWindowID wnd, int x, int y,
                          XPLMMouseStatus status, void *) {
  int left, top, right, bottom;
  XPLMGetWindowGeometry(wnd, &left, &top, &right, &bottom);
  ImGuiIO &io = ImGui::GetIO();
  io.AddMousePosEvent(static_cast<float>(x - left),
                      static_cast<float>(top - y));
  if (status == xplm_MouseDown)
    io.AddMouseButtonEvent(1, true);
  if (status == xplm_MouseUp)
    io.AddMouseButtonEvent(1, false);
  return 1;
}

static int wnd_wheel_cb(XPLMWindowID wnd, int x, int y, int, int clicks,
                         void *) {
  int left, top, right, bottom;
  XPLMGetWindowGeometry(wnd, &left, &top, &right, &bottom);
  ImGui::GetIO().AddMousePosEvent(static_cast<float>(x - left),
                                  static_cast<float>(top - y));
  ImGui::GetIO().AddMouseWheelEvent(0.0f, static_cast<float>(clicks));
  return 1;
}

static XPLMCursorStatus wnd_cursor_cb(XPLMWindowID, int, int, void *) {
  return xplm_CursorDefault;
}

static void wnd_key_cb(XPLMWindowID, char key, XPLMKeyFlags flags, char vkey,
                        void *, int losing_focus) {
  if (losing_focus)
    return;
  ImGuiIO &io = ImGui::GetIO();
  if (!(flags & xplm_DownFlag))
    return;
  if (key >= 32 && key < 127)
    io.AddInputCharacter(static_cast<unsigned>(key));
  if (vkey == XPLM_VK_BACK)
    io.AddKeyEvent(ImGuiKey_Backspace, true);
  if (vkey == XPLM_VK_DELETE)
    io.AddKeyEvent(ImGuiKey_Delete, true);
  if (vkey == XPLM_VK_RETURN)
    io.AddKeyEvent(ImGuiKey_Enter, true);
  if (vkey == XPLM_VK_ESCAPE) {
    visible = false;
    if (window_id) {
      XPLMSetWindowIsVisible(window_id, 0);
      XPLMTakeKeyboardFocus(nullptr);
    }
  }
}

// ── Draw phase callback (ImGui rendering) ────────────────────────

static int draw_phase_cb(XPLMDrawingPhase, int, void *) {
  if (!visible)
    return 1;

  int gl, gt, gr, gb;
  XPLMGetScreenBoundsGlobal(&gl, &gt, &gr, &gb);
  int sw = gr - gl;
  int sh = gt - gb;
  if (sw <= 0 || sh <= 0)
    return 1;

  // Keep capture window sized to full screen
  if (window_id) {
    int wl, wt, wr, wb;
    XPLMGetWindowGeometry(window_id, &wl, &wt, &wr, &wb);
    if (wl != gl || wb != gb || wr != gr || wt != gt)
      XPLMSetWindowGeometry(window_id, gl, gt, gr, gb);
  }

  // Save GL state
  GLint prev_viewport[4];
  glGetIntegerv(GL_VIEWPORT, prev_viewport);
  glPushAttrib(GL_TRANSFORM_BIT | GL_ENABLE_BIT | GL_COLOR_BUFFER_BIT |
               GL_DEPTH_BUFFER_BIT | GL_SCISSOR_BIT | GL_TEXTURE_BIT);
  glMatrixMode(GL_PROJECTION);
  glPushMatrix();
  glMatrixMode(GL_MODELVIEW);
  glPushMatrix();

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_LIGHTING);
  glEnable(GL_BLEND);
  glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

  glViewport(0, 0, sw, sh);
  glMatrixMode(GL_PROJECTION);
  glLoadIdentity();
  glOrtho(0, sw, sh, 0, -1, 1); // top-left origin for ImGui
  glMatrixMode(GL_MODELVIEW);
  glLoadIdentity();

  // ImGui frame setup
  ImGuiIO &io = ImGui::GetIO();
  double now = get_xp_time();
  io.DeltaTime = static_cast<float>(std::max(now - last_frame_time_, 0.001));
  last_frame_time_ = now;
  io.DisplaySize = ImVec2(static_cast<float>(sw), static_cast<float>(sh));

  // Track mouse position every frame (hover support)
  int gmx, gmy;
  XPLMGetMouseLocationGlobal(&gmx, &gmy);
  io.AddMousePosEvent(static_cast<float>(gmx - gl),
                      static_cast<float>(gt - gmy));

  ImGui_ImplOpenGL2_NewFrame();
  ImGui::NewFrame();

  // Center the ATC window
  float win_w = 500.0f, win_h = 450.0f;
  ImGui::SetNextWindowPos(
      ImVec2((static_cast<float>(sw) - win_w) * 0.5f,
             (static_cast<float>(sh) - win_h) * 0.5f),
      ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(win_w, win_h), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSizeConstraints(ImVec2(400, 300), ImVec2(1920, 1080));

  bool open = visible;
  if (ImGui::Begin("Welly's ATC##main", &open, ImGuiWindowFlags_NoCollapse)) {
    if (ImGui::BeginTabBar("MainTabs")) {
      if (ImGui::BeginTabItem("Status")) {
        draw_status_tab();
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Transcript")) {
        draw_transcript_tab();
        ImGui::EndTabItem();
      }
      if (ImGui::BeginTabItem("Settings")) {
        draw_settings_tab();
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
  }
  ImGui::End();

  if (!open) {
    visible = false;
    if (window_id) {
      XPLMSetWindowIsVisible(window_id, 0);
      XPLMTakeKeyboardFocus(nullptr);
    }
  }

  ImGui::Render();
  ImGui_ImplOpenGL2_RenderDrawData(ImGui::GetDrawData());

  // Restore GL state
  glMatrixMode(GL_PROJECTION);
  glPopMatrix();
  glMatrixMode(GL_MODELVIEW);
  glPopMatrix();
  glPopAttrib();
  glViewport(prev_viewport[0], prev_viewport[1], prev_viewport[2],
             prev_viewport[3]);

  return 1;
}

// ── Public lifecycle ─────────────────────────────────────────────

void init() {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();

  ImGuiIO &io = ImGui::GetIO();
  io.IniFilename = nullptr;
  io.LogFilename = nullptr;
  io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

  ImGui::StyleColorsDark();
  auto &style = ImGui::GetStyle();
  style.WindowRounding = 6.0f;
  style.FrameRounding = 3.0f;
  style.WindowPadding = ImVec2(8, 6);

  ImGui_ImplOpenGL2_Init();
  last_frame_time_ = get_xp_time();

  XPLMRegisterDrawCallback(draw_phase_cb, xplm_Phase_Window, 1, nullptr);
}

void stop() {
  XPLMUnregisterDrawCallback(draw_phase_cb, xplm_Phase_Window, 1, nullptr);

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

  if (visible && !window_id) {
    // Create full-screen invisible capture window
    int gl, gt, gr, gb;
    XPLMGetScreenBoundsGlobal(&gl, &gt, &gr, &gb);

    XPLMCreateWindow_t p{};
    p.structSize = sizeof(p);
    p.left = gl;
    p.bottom = gb;
    p.right = gr;
    p.top = gt;
    p.visible = 1;
    p.drawWindowFunc = wnd_draw_cb;
    p.handleMouseClickFunc = wnd_mouse_cb;
    p.handleKeyFunc = wnd_key_cb;
    p.handleCursorFunc = wnd_cursor_cb;
    p.handleMouseWheelFunc = wnd_wheel_cb;
    p.handleRightClickFunc = wnd_rclick_cb;
    p.refcon = nullptr;
    p.decorateAsFloatingWindow = xplm_WindowDecorationNone;
    p.layer = xplm_WindowLayerFloatingWindows;
    window_id = XPLMCreateWindowEx(&p);

    if (settings::debug_logging()) {
      char dbg[256];
      std::snprintf(dbg, sizeof(dbg),
                    "[xp_wellys_atc] Capture window created: "
                    "bounds(%d,%d,%d,%d) wnd=%p\n",
                    gl, gt, gr, gb, static_cast<void *>(window_id));
      XPLMDebugString(dbg);
    }
  }

  if (window_id) {
    XPLMSetWindowIsVisible(window_id, visible ? 1 : 0);
    if (visible) {
      XPLMBringWindowToFront(window_id);
      XPLMTakeKeyboardFocus(window_id);
    } else {
      XPLMTakeKeyboardFocus(nullptr); // release focus
    }
  }
}

void draw() {
  // Rendering now handled by draw_phase_cb
}

} // namespace atc_ui
