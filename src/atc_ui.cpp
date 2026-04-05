#include "atc_ui.hpp"
#include "atc_session.hpp"
#include "atc_state_machine.hpp"
#include "audio_player.hpp"
#include "intent_parser.hpp"
#include "ptt_input.hpp"
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
static char callsign_raw_buf[64] = {};
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
static bool window_pos_reset_pending_ = false;
static float geometry_save_timer_ = 0.0f;
static constexpr float kGeometrySaveDelay = 0.5f; // save 0.5s after last change

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
  ImGui::Text("COM%d: %.3f MHz (%s)", ctx.active_com, active_freq,
              xplane_context::frequency_type_name(ctx.frequency_type));

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

  // Session stats
  ImGui::Separator();
  ImGui::Text("Session: %d transcriptions, %d API calls",
              atc_session::total_transcriptions(),
              atc_session::total_api_calls());

  // Warning indicators
  if (!settings::api_key_saved()) {
    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.0f, 1.0f),
                       "\xe2\x9a\xa0 API key not set");
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
    std::string freq_tag = entry.frequency.empty() ? "" : " " + entry.frequency;
    if (entry.is_pilot) {
      std::snprintf(line, sizeof(line), "[%02d:%02d%s] You: %s", mins, secs,
                    freq_tag.c_str(), entry.text.c_str());
      ImGui::TextUnformatted(line);
    } else {
      const auto &cx = xplane_context::get();
      std::string prefix = cx.nearest_airport_id.empty()
                               ? "ATC"
                               : cx.nearest_airport_id + " ATC";
      std::snprintf(line, sizeof(line), "[%02d:%02d%s] %s: %s", mins, secs,
                    freq_tag.c_str(), prefix.c_str(), entry.text.c_str());
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

static const char *vk_name(int vk) {
  // Common XPLM virtual key codes
  switch (vk) {
  case 0x08:
    return "Backspace";
  case 0x09:
    return "Tab";
  case 0x0D:
    return "Enter";
  case 0x1B:
    return "Escape";
  case 0x20:
    return "Space";
  case 0x25:
    return "Left";
  case 0x26:
    return "Up";
  case 0x27:
    return "Right";
  case 0x28:
    return "Down";
  case 0x2E:
    return "Delete";
  default:
    break;
  }
  // 0-9
  if (vk >= 0x30 && vk <= 0x39) {
    static char num[2];
    num[0] = static_cast<char>(vk);
    num[1] = '\0';
    return num;
  }
  // A-Z
  if (vk >= 0x41 && vk <= 0x5A) {
    static char letter[2];
    letter[0] = static_cast<char>(vk);
    letter[1] = '\0';
    return letter;
  }
  // F1-F12
  if (vk >= 0x70 && vk <= 0x7B) {
    static char fkey[4];
    std::snprintf(fkey, sizeof(fkey), "F%d", vk - 0x70 + 1);
    return fkey;
  }
  static char hex[16];
  std::snprintf(hex, sizeof(hex), "VK 0x%02X", vk);
  return hex;
}

static constexpr float kCaptureTimeout = 10.0f;

static void draw_ptt_binding() {
  ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.0f, 1.0f), "Push-to-Talk Binding");
  ImGui::TextDisabled("(X-Plane command: xp_wellys_atc/ptt also works)");
  ImGui::Spacing();

  auto mode = ptt_input::capture_mode();

  // Check for capture result
  if (mode != ptt_input::CaptureMode::NONE) {
    int result = ptt_input::poll_capture_result();
    if (result >= 0) {
      if (mode == ptt_input::CaptureMode::KEYBOARD) {
        settings::set_ptt_key_vk(result);
        settings::save();
      } else {
        settings::set_ptt_joystick_button(result);
        settings::save();
      }
      mode = ptt_input::CaptureMode::NONE; // capture done
    }
  }

  // ── Keyboard binding ──
  int vk = settings::ptt_key_vk();
  if (vk >= 0) {
    ImGui::Text("Keyboard: %s", vk_name(vk));
  } else {
    ImGui::TextDisabled("Keyboard: (not bound)");
  }
  ImGui::SameLine();
  if (mode == ptt_input::CaptureMode::KEYBOARD) {
    float elapsed = ptt_input::capture_elapsed();
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f),
                       "Press any key... (%.0fs)", kCaptureTimeout - elapsed);
    ImGui::SameLine();
    if (ImGui::SmallButton("Cancel##key")) {
      ptt_input::cancel_capture();
    }
  } else {
    if (ImGui::SmallButton("Bind key")) {
      ptt_input::start_capture(ptt_input::CaptureMode::KEYBOARD);
    }
    if (vk >= 0) {
      ImGui::SameLine();
      if (ImGui::SmallButton("Clear##key")) {
        settings::set_ptt_key_vk(-1);
        settings::save();
      }
    }
  }

  // ── Joystick button binding ──
  int btn = settings::ptt_joystick_button();
  if (btn >= 0) {
    ImGui::Text("Joystick: Button %d", btn);
  } else {
    ImGui::TextDisabled("Joystick: (not bound)");
  }
  ImGui::SameLine();
  if (mode == ptt_input::CaptureMode::JOYSTICK) {
    float elapsed = ptt_input::capture_elapsed();
    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.0f, 1.0f),
                       "Press any button... (%.0fs)",
                       kCaptureTimeout - elapsed);
    ImGui::SameLine();
    if (ImGui::SmallButton("Cancel##btn")) {
      ptt_input::cancel_capture();
    }
  } else {
    if (ImGui::SmallButton("Bind button")) {
      ptt_input::start_capture(ptt_input::CaptureMode::JOYSTICK);
    }
    if (btn >= 0) {
      ImGui::SameLine();
      if (ImGui::SmallButton("Clear##btn")) {
        settings::set_ptt_joystick_button(-1);
        settings::save();
      }
    }
  }

  ImGui::Separator();
}

static void draw_settings_tab() {
  // One-time init of buffers from settings
  if (!buffers_initialized) {
    std::strncpy(callsign_raw_buf, settings::pilot_callsign_raw().c_str(),
                 sizeof(callsign_raw_buf) - 1);
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
    FILE *fp = popen("pbpaste",
                     "r"); // NOLINT(cert-env33-c,bugprone-command-processor)
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

  // PTT Binding
  draw_ptt_binding();

  // Pilot callsign — raw registration input + phonetic preview
  if (ImGui::InputText("Callsign (Registration)", callsign_raw_buf,
                       sizeof(callsign_raw_buf))) {
    settings::set_pilot_callsign_raw(callsign_raw_buf);
  }
  std::string phonetic = settings::pilot_callsign();
  if (!phonetic.empty()) {
    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f), "  %s",
                       phonetic.c_str());
  } else {
    ImGui::TextDisabled("  (enter registration, e.g. HB-WRO or N342B4)");
  }

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
    settings::save();
  }

  // About section
  ImGui::Separator();
  ImGui::Spacing();
  ImGui::TextDisabled("About");
#ifdef XP_WELLYS_ATC_VERSION
  ImGui::Text("Welly's ATC v%s", XP_WELLYS_ATC_VERSION);
#else
  ImGui::Text("Welly's ATC (dev build)");
#endif
  ImGui::Text("AI-powered ATC for X-Plane 12 VFR");
  ImGui::TextDisabled("github.com/rwellinger/xp_welly_atc");
}

// ── XPLM window callbacks (input capture only) ──────────────────

static void wnd_draw_cb(XPLMWindowID, void *) {
  // Nothing — rendering happens in the draw phase callback
}

static int wnd_mouse_cb(XPLMWindowID wnd, int x, int y, XPLMMouseStatus status,
                        void *) {
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

static int wnd_rclick_cb(XPLMWindowID wnd, int x, int y, XPLMMouseStatus status,
                         void *) {
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

  // Window position/size — load from settings or center
  if (window_pos_reset_pending_) {
    // Force re-center on next frame
    float def_w = 500.0f, def_h = 450.0f;
    ImGui::SetNextWindowPos(ImVec2((static_cast<float>(sw) - def_w) * 0.5f,
                                   (static_cast<float>(sh) - def_h) * 0.5f),
                            ImGuiCond_Always);
    ImGui::SetNextWindowSize(ImVec2(def_w, def_h), ImGuiCond_Always);
    window_pos_reset_pending_ = false;
  } else {
    float sx = settings::window_x();
    float sy = settings::window_y();
    float sw_s = settings::window_w();
    float sh_s = settings::window_h();
    if (sx >= 0.0f && sy >= 0.0f) {
      ImGui::SetNextWindowPos(ImVec2(sx, sy), ImGuiCond_FirstUseEver);
    } else {
      float def_w = 500.0f, def_h = 450.0f;
      ImGui::SetNextWindowPos(ImVec2((static_cast<float>(sw) - def_w) * 0.5f,
                                     (static_cast<float>(sh) - def_h) * 0.5f),
                              ImGuiCond_FirstUseEver);
    }
    if (sw_s > 0.0f && sh_s > 0.0f) {
      ImGui::SetNextWindowSize(ImVec2(sw_s, sh_s), ImGuiCond_FirstUseEver);
    } else {
      ImGui::SetNextWindowSize(ImVec2(500.0f, 450.0f), ImGuiCond_FirstUseEver);
    }
  }
  ImGui::SetNextWindowSizeConstraints(ImVec2(400, 300), ImVec2(1920, 1080));

  bool open = visible;
#ifdef XP_WELLYS_ATC_VERSION
  static const std::string window_title =
      std::string("Welly's ATC v") + XP_WELLYS_ATC_VERSION + "##main";
#else
  static const std::string window_title = "Welly's ATC##main";
#endif
  if (ImGui::Begin(window_title.c_str(), &open, ImGuiWindowFlags_NoCollapse)) {
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

    // Save window geometry when moved/resized (debounced)
    ImVec2 pos = ImGui::GetWindowPos();
    ImVec2 size = ImGui::GetWindowSize();
    float prev_x = settings::window_x();
    float prev_y = settings::window_y();
    float prev_w = settings::window_w();
    float prev_h = settings::window_h();
    if (pos.x != prev_x || pos.y != prev_y || size.x != prev_w ||
        size.y != prev_h) {
      settings::set_window_geometry(pos.x, pos.y, size.x, size.y);
      geometry_save_timer_ = kGeometrySaveDelay;
    }
    if (geometry_save_timer_ > 0.0f) {
      geometry_save_timer_ -= ImGui::GetIO().DeltaTime;
      if (geometry_save_timer_ <= 0.0f) {
        settings::save();
        geometry_save_timer_ = 0.0f;
      }
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

void reset_window_position() {
  settings::reset_window_geometry();
  window_pos_reset_pending_ = true;
  // Open the window so the user can see it
  if (!visible)
    toggle();
}

} // namespace atc_ui
