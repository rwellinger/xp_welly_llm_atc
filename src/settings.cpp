#include "settings.hpp"

#include <fstream>
#include <string>
#include <sys/stat.h>

#include <XPLMPlugin.h>
#include <XPLMUtilities.h>
#include <json.hpp>

#if defined(__APPLE__)
#include <Security/Security.h>
#endif

namespace settings {

using json = nlohmann::json;

static json cfg;
static std::string cached_api_key;
static std::string data_dir_path;

static const char *kKeychainService = "xp_wellys_atc";
static const char *kKeychainAccount = "openai_api_key";

static json default_config() {
  return {{"api_key_saved", false},
          {"ptt_key_vk", -1},
          {"ptt_joystick_button", -1},
          {"tts_voice", "onyx"},
          {"tts_model", "tts-1"},
          {"whisper_model", "whisper-1"},
          {"gpt_model", "gpt-4o-mini"},
          {"gpt_fallback_enabled", true},
          {"pilot_callsign_raw", ""},
          {"pilot_callsign", ""},
          {"active_com", 1},
          {"volume", 1.0},
          {"debug_logging", false},
          {"audio_output_device", ""},
          {"window_x", -1.0},
          {"window_y", -1.0},
          {"window_w", -1.0},
          {"window_h", -1.0}};
}

void init() {
  // Resolve plugin path to find data/ directory
  // Installed: .../plugins/xp_wellys_atc/mac_x64/xp_wellys_atc.xpl
  // We need to go up 2 levels to reach the plugin root
  char plugin_path_raw[2048] = {};
  XPLMGetPluginInfo(XPLMGetMyID(), nullptr, plugin_path_raw, nullptr, nullptr);

  std::string path_str(plugin_path_raw);
#if defined(__APPLE__)
  // macOS may return an HFS path (colon-separated) — convert to POSIX
  if (path_str.find(':') != std::string::npos &&
      path_str.find('/') == std::string::npos) {
    auto colon = path_str.find(':');
    std::string posix = path_str.substr(colon + 1);
    for (char &c : posix)
      if (c == ':')
        c = '/';
    path_str = "/" + posix;
  }
#endif

  // Strip filename → directory, then strip platform dir (mac_x64/)
  auto pos = path_str.rfind('/');
  if (pos != std::string::npos) {
    pos = path_str.rfind('/', pos - 1);
  }
  if (pos != std::string::npos) {
    data_dir_path = path_str.substr(0, pos) + "/data";
  } else {
    data_dir_path = "data";
  }

  mkdir(data_dir_path.c_str(), 0755);

  std::string json_path = data_dir_path + "/settings.json";
  std::ifstream in(json_path);
  if (in.good()) {
    try {
      in >> cfg;
      // Merge any missing defaults
      json defaults = default_config();
      for (auto &[key, value] : defaults.items()) {
        if (!cfg.contains(key)) {
          cfg[key] = value;
        }
      }
    } catch (...) {
      XPLMDebugString("[xp_wellys_atc] Warning: failed to parse settings.json, "
                      "using defaults\n");
      cfg = default_config();
    }
  } else {
    cfg = default_config();
    save();
  }

  if (cfg.value("api_key_saved", false)) {
    cached_api_key = load_api_key();
    if (cached_api_key.empty()) {
      XPLMDebugString("[xp_wellys_atc] Warning: api_key_saved flag set but "
                      "Keychain load failed\n");
      cfg["api_key_saved"] = false;
    }
  }

  XPLMDebugString("[xp_wellys_atc] Settings loaded\n");
}

void stop() {}

void save() {
  std::string json_path = data_dir_path + "/settings.json";
  std::ofstream out(json_path);
  if (out.good()) {
    out << cfg.dump(2) << std::endl;
  } else {
    XPLMDebugString("[xp_wellys_atc] Error: failed to write settings.json\n");
  }
}

// --- Keychain ---

#if defined(__APPLE__)

bool save_api_key(const std::string &key) {
  SecKeychainItemRef item_ref = nullptr;
  OSStatus status = SecKeychainFindGenericPassword(
      nullptr, static_cast<UInt32>(strlen(kKeychainService)), kKeychainService,
      static_cast<UInt32>(strlen(kKeychainAccount)), kKeychainAccount, nullptr,
      nullptr, &item_ref);

  if (status == errSecSuccess && item_ref) {
    status = SecKeychainItemModifyAttributesAndData(
        item_ref, nullptr, static_cast<UInt32>(key.size()), key.c_str());
    CFRelease(item_ref);
  } else {
    status = SecKeychainAddGenericPassword(
        nullptr, static_cast<UInt32>(strlen(kKeychainService)),
        kKeychainService, static_cast<UInt32>(strlen(kKeychainAccount)),
        kKeychainAccount, static_cast<UInt32>(key.size()), key.c_str(),
        nullptr);
  }

  if (status == errSecSuccess) {
    cached_api_key = key;
    cfg["api_key_saved"] = true;
    save();
    return true;
  }
  XPLMDebugString(
      "[xp_wellys_atc] Error: failed to save API key to Keychain\n");
  return false;
}

std::string load_api_key() {
  UInt32 pw_len = 0;
  void *pw_data = nullptr;
  OSStatus status = SecKeychainFindGenericPassword(
      nullptr, static_cast<UInt32>(strlen(kKeychainService)), kKeychainService,
      static_cast<UInt32>(strlen(kKeychainAccount)), kKeychainAccount, &pw_len,
      &pw_data, nullptr);

  if (status == errSecSuccess && pw_data) {
    std::string result(static_cast<char *>(pw_data), pw_len);
    SecKeychainItemFreeContent(nullptr, pw_data);
    return result;
  }
  return "";
}

void delete_api_key() {
  SecKeychainItemRef item_ref = nullptr;
  OSStatus status = SecKeychainFindGenericPassword(
      nullptr, static_cast<UInt32>(strlen(kKeychainService)), kKeychainService,
      static_cast<UInt32>(strlen(kKeychainAccount)), kKeychainAccount, nullptr,
      nullptr, &item_ref);

  if (status == errSecSuccess && item_ref) {
    SecKeychainItemDelete(item_ref);
    CFRelease(item_ref);
  }
  cached_api_key.clear();
  cfg["api_key_saved"] = false;
  save();
}

#else

bool save_api_key(const std::string &) { return false; }
std::string load_api_key() { return ""; }
void delete_api_key() {}

#endif

std::string get_api_key() { return cached_api_key; }

// --- Getters ---

bool api_key_saved() { return cfg.value("api_key_saved", false); }
int ptt_key_vk() { return cfg.value("ptt_key_vk", -1); }
int ptt_joystick_button() { return cfg.value("ptt_joystick_button", -1); }
std::string tts_voice() { return cfg.value("tts_voice", std::string("onyx")); }
std::string tts_model() { return cfg.value("tts_model", std::string("tts-1")); }
std::string whisper_model() {
  return cfg.value("whisper_model", std::string("whisper-1"));
}
std::string gpt_model() {
  return cfg.value("gpt_model", std::string("gpt-4o-mini"));
}
bool gpt_fallback_enabled() { return cfg.value("gpt_fallback_enabled", true); }
std::string pilot_callsign_raw() {
  return cfg.value("pilot_callsign_raw", std::string(""));
}
std::string pilot_callsign() {
  return cfg.value("pilot_callsign", std::string(""));
}
int active_com() { return cfg.value("active_com", 1); }
float volume() { return cfg.value("volume", 1.0f); }
bool debug_logging() { return cfg.value("debug_logging", false); }
std::string audio_output_device() {
  return cfg.value("audio_output_device", std::string(""));
}

// --- Setters ---

void set_tts_voice(const std::string &v) { cfg["tts_voice"] = v; }
// ── ICAO phonetic alphabet conversion ───────────────────────────

static const char *phonetic_letter(char c) {
  static const char *letters[] = {
      "Alpha",  "Bravo",   "Charlie", "Delta",  "Echo",   "Foxtrot", "Golf",
      "Hotel",  "India",   "Juliet",  "Kilo",   "Lima",   "Mike",    "November",
      "Oscar",  "Papa",    "Quebec",  "Romeo",  "Sierra", "Tango",   "Uniform",
      "Victor", "Whiskey", "X-Ray",   "Yankee", "Zulu"};
  if (c >= 'A' && c <= 'Z')
    return letters[c - 'A'];
  if (c >= 'a' && c <= 'z')
    return letters[c - 'a'];
  return nullptr;
}

static const char *phonetic_digit(char c) {
  static const char *digits[] = {"Zero", "One", "Two",   "Three", "Four",
                                 "Five", "Six", "Seven", "Eight", "Niner"};
  if (c >= '0' && c <= '9')
    return digits[c - '0'];
  return nullptr;
}

std::string to_icao_phonetic(const std::string &raw) {
  std::string result;
  for (char c : raw) {
    const char *word = phonetic_letter(c);
    if (!word)
      word = phonetic_digit(c);
    if (word) {
      if (!result.empty())
        result += ' ';
      result += word;
    }
    // Skip dashes, spaces, and other non-alphanumeric chars
  }
  return result;
}

void set_pilot_callsign_raw(const std::string &raw) {
  cfg["pilot_callsign_raw"] = raw;
  cfg["pilot_callsign"] = to_icao_phonetic(raw);
}
void set_volume(float v) { cfg["volume"] = v; }
void set_gpt_fallback_enabled(bool v) { cfg["gpt_fallback_enabled"] = v; }
void set_debug_logging(bool v) { cfg["debug_logging"] = v; }
void set_active_com(int com) { cfg["active_com"] = com; }
void set_audio_output_device(const std::string &uid) {
  cfg["audio_output_device"] = uid;
}
void set_ptt_key_vk(int vk) { cfg["ptt_key_vk"] = vk; }
void set_ptt_joystick_button(int btn) { cfg["ptt_joystick_button"] = btn; }

float window_x() { return cfg.value("window_x", -1.0f); }
float window_y() { return cfg.value("window_y", -1.0f); }
float window_w() { return cfg.value("window_w", -1.0f); }
float window_h() { return cfg.value("window_h", -1.0f); }
void set_window_geometry(float x, float y, float w, float h) {
  cfg["window_x"] = x;
  cfg["window_y"] = y;
  cfg["window_w"] = w;
  cfg["window_h"] = h;
}
void reset_window_geometry() {
  cfg["window_x"] = -1.0;
  cfg["window_y"] = -1.0;
  cfg["window_w"] = -1.0;
  cfg["window_h"] = -1.0;
  save();
}

} // namespace settings
