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

#include "atc/atis_generator.hpp"
#include "atc/atc_state_machine.hpp"
#include "core/logging.hpp"
#include "persistence/settings.hpp"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>

namespace {
float monotonic_seconds() {
  using clock = std::chrono::steady_clock;
  static const auto t0 = clock::now();
  auto dt = std::chrono::duration_cast<std::chrono::duration<float>>(
      clock::now() - t0);
  return dt.count();
}
} // namespace

namespace atis_generator {

static char letter_ = 'A';

// Snapshot of last ATIS-relevant state for change detection
static std::string last_runway_;
static float last_wind_dir_ = 0.0f;
static float last_qnh_inhg_ = 29.92f;
static int last_vis_category_ = 0;        // 0= >10km, 1= 5-10km, 2= <5km
static float last_increment_time_ = 0.0f; // cooldown for letter changes
static bool baseline_initialized_ = false;

static const char *kLetterNames[] = {
    "Alpha",  "Bravo",   "Charlie", "Delta",  "Echo",   "Foxtrot", "Golf",
    "Hotel",  "India",   "Juliet",  "Kilo",   "Lima",   "Mike",    "November",
    "Oscar",  "Papa",    "Quebec",  "Romeo",  "Sierra", "Tango",   "Uniform",
    "Victor", "Whiskey", "X-ray",   "Yankee", "Zulu"};

// NATO/ICAO spelling alphabet — used only on the DE path. Differs from
// kLetterNames in "Alfa" / "Juliett" / "X-Ray" per ICAO Annex 10. The
// M3 normalizer's swap_information_alpha pass already rewrites a stray
// "Alpha" to "Alfa" downstream, but emitting NATO directly keeps the
// pre-TTS string self-consistent and lets the test fixtures assert on
// the raw output before normalization.
static const char *kLetterNamesDE[] = {
    "Alfa",   "Bravo",   "Charlie", "Delta",    "Echo",   "Foxtrot", "Golf",
    "Hotel",  "India",   "Juliett", "Kilo",     "Lima",   "Mike",    "November",
    "Oscar",  "Papa",    "Quebec",  "Romeo",    "Sierra", "Tango",   "Uniform",
    "Victor", "Whiskey", "X-Ray",   "Yankee",   "Zulu"};

static int visibility_category(float vis_m) {
  if (vis_m >= 10000.0f)
    return 0;
  if (vis_m >= 5000.0f)
    return 1;
  return 2;
}

static std::string format_visibility(float vis_m) {
  const std::string region = settings::atc_profile();
  if (region == "US") {
    // Statute miles, FAA AIM. 1 SM = 1609.344 m. Cap at "10 or more" like ATIS.
    float sm = vis_m / 1609.344f;
    if (sm >= 10.0f)
      return "1 0 statute miles";
    int sm_int = static_cast<int>(std::round(sm));
    if (sm_int < 1)
      sm_int = 1;
    return std::to_string(sm_int) + " statute miles";
  }
  if (region == "DE") {
    // BZF: km ab 1 km, sonst Meter. Ueber-10-km Kappung wie ICAO ATIS.
    if (vis_m >= 10000.0f)
      return "ueber 10 Kilometer";
    if (vis_m >= 1000.0f) {
      int km = static_cast<int>(vis_m / 1000.0f);
      return std::to_string(km) + " Kilometer";
    }
    int m = static_cast<int>(vis_m);
    return std::to_string(m) + " Meter";
  }
  if (vis_m >= 10000.0f)
    return "10 kilometers or more";
  if (vis_m >= 1000.0f) {
    int km = static_cast<int>(vis_m / 1000.0f);
    return std::to_string(km) + " kilometers";
  }
  int m = static_cast<int>(vis_m);
  return std::to_string(m) + " meters";
}

static std::string format_clouds(int cloud_type, float cloud_base_ft) {
  // X-Plane sometimes reports a non-zero cloud_type alongside a 0 ft
  // cloud_base when the sky is effectively clear (sensor quirk). Treat
  // any base below 100 ft as clear sky rather than emitting an absurd
  // "few clouds at 0 feet" / "wenige Wolken in 0 Fuss" line.
  const bool effectively_clear = cloud_type <= 0 || cloud_base_ft < 100.0f;
  if (settings::atc_profile() == "DE") {
    if (effectively_clear)
      return "Wolkenlos.";
    const char *coverage = "wenige Wolken";
    switch (cloud_type) {
    case 1:
      coverage = "wenige Wolken";
      break;
    case 2:
      coverage = "vereinzelte Wolken";
      break;
    case 3:
      coverage = "aufgelockerte Wolken";
      break;
    case 4:
      coverage = "bedeckt";
      break;
    default:
      break;
    }
    // AIP folgt: VFR-Wolkenbasis in Fuss, auf 100 ft gerundet. Der M3-
    // Normalizer (expand_altitudes) rendert "3500 Fuss" als
    // "drei tausend fuenfhundert Fuss".
    int base = static_cast<int>(std::round(cloud_base_ft / 100.0f)) * 100;
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s in %d Fuss.", coverage, base);
    return buf;
  }
  if (effectively_clear)
    return "Sky clear.";
  const char *coverage = "Few";
  switch (cloud_type) {
  case 1:
    coverage = "Few";
    break;
  case 2:
    coverage = "Scattered";
    break;
  case 3:
    coverage = "Broken";
    break;
  case 4:
    coverage = "Overcast";
    break;
  default:
    break;
  }
  int base = static_cast<int>(std::round(cloud_base_ft / 100.0f)) * 100;
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%s clouds at %d feet.", coverage, base);
  return buf;
}

static std::string format_wind(float dir, float spd) {
  if (settings::atc_profile() == "DE") {
    if (spd < 3.0f)
      return "still";
    char buf[64];
    // Anker "Grad" und "Knoten" bleiben — Normalizer-Pass 6
    // (expand_wind) erkennt das Muster und stellt beide Zahlen
    // ziffernweise.
    std::snprintf(buf, sizeof(buf), "%.0f Grad %.0f Knoten", dir, spd);
    return buf;
  }
  if (spd < 3.0f)
    return "calm";
  char buf[64];
  std::snprintf(buf, sizeof(buf), "%03.0f degrees %02.0f knots", dir, spd);
  return buf;
}

static std::string format_qnh(float inhg) {
  int hpa = static_cast<int>(std::round(inhg * 33.8639f));
  return std::to_string(hpa);
}

static std::string format_altimeter(float inhg) {
  // Two decimal places, spoken as "two niner point niner two".
  char buf[16];
  std::snprintf(buf, sizeof(buf), "%.2f", inhg);
  return buf;
}

void init() {
  letter_ = 'A';
  last_runway_.clear();
  last_wind_dir_ = 0.0f;
  last_qnh_inhg_ = 29.92f;
  last_vis_category_ = 0;
  baseline_initialized_ = false;
}

void stop() {
  letter_ = 'A';
  last_increment_time_ = 0.0f;
}

char current_letter() { return letter_; }

void check_for_update(const xplane_context::XPlaneContext &ctx) {
  // While ATC has cleared the pilot for a specific runway, freeze the
  // ATIS letter. Real-world ATIS does not update mid-approach; once the
  // dialog returns to IDLE (lock released), the next frame can resume
  // change tracking against the current wind/runway.
  if (!atc_state_machine::assigned_runway().empty())
    return;

  int vis_cat = visibility_category(ctx.visibility_m);

  // First valid sample: seed the baseline without incrementing the letter.
  if (!baseline_initialized_) {
    if (ctx.active_runway.empty())
      return; // wait until we have a runway (airport context loaded)
    last_runway_ = ctx.active_runway;
    last_wind_dir_ = ctx.wind_direction_deg;
    last_qnh_inhg_ = ctx.qnh_inhg;
    last_vis_category_ = vis_cat;
    baseline_initialized_ = true;
    return;
  }

  bool changed = false;

  // Active runway changed
  if (!ctx.active_runway.empty() && ctx.active_runway != last_runway_)
    changed = true;

  // Wind direction changed > 30 degrees (only meaningful if wind is not calm).
  // Matches runway-selection calm threshold (3 kt).
  if (ctx.wind_speed_kt >= 3.0f) {
    float wind_diff = std::fabs(
        std::fmod(ctx.wind_direction_deg - last_wind_dir_ + 540.0f, 360.0f) -
        180.0f);
    if (wind_diff > 30.0f)
      changed = true;
  }

  // QNH changed > 1 hPa (~0.0295 inHg)
  if (std::fabs(ctx.qnh_inhg - last_qnh_inhg_) > 0.0295f)
    changed = true;

  // Visibility category changed
  if (vis_cat != last_vis_category_)
    changed = true;

  if (!changed)
    return;

  float now = monotonic_seconds();
  if (now - last_increment_time_ < 300.0f) // 5 min cooldown
    return;

  // Commit new baseline on successful increment only.
  last_runway_ = ctx.active_runway;
  last_wind_dir_ = ctx.wind_direction_deg;
  last_qnh_inhg_ = ctx.qnh_inhg;
  last_vis_category_ = vis_cat;
  last_increment_time_ = now;
  letter_ = static_cast<char>('A' + (letter_ - 'A' + 1) % 26);
  logging::info("ATIS letter incremented to %c (%s)", letter_,
                kLetterNames[letter_ - 'A']);
}

static std::string
generate_atis_text_de(const xplane_context::XPlaneContext &ctx) {
  std::string airport =
      !ctx.nearest_airport_name.empty()
          ? ctx.nearest_airport_name
          : (!ctx.nearest_airport_id.empty() ? ctx.nearest_airport_id
                                             : "Flugplatz");
  const char *letter_name = kLetterNamesDE[letter_ - 'A'];
  std::string eff = atc_state_machine::effective_runway(ctx);
  std::string runway = eff.empty() ? "unbekannt" : eff;

  std::string text;
  text += airport + " Information " + letter_name + " aktuell. ";
  text += "Piste " + runway + " in Betrieb. ";
  text +=
      "Wind " + format_wind(ctx.wind_direction_deg, ctx.wind_speed_kt) + ". ";
  text += "Sicht " + format_visibility(ctx.visibility_m) + ". ";
  text += format_clouds(ctx.cloud_type, ctx.cloud_base_ft_msl) + " ";
  // Temperatur/Taupunkt als Rohziffern: echte DE-ATIS spricht
  // "achtzehn" / "zwoelf", espeak-ng-DE liest 18 / 12 nativ als
  // Zahlworte. Bewusst kein Normalizer-Pass dafuer (kein BZF-
  // ziffernweise fuer Temperaturen).
  text += "Temperatur " + std::to_string(static_cast<int>(ctx.temperature_c)) +
          ", Taupunkt " + std::to_string(static_cast<int>(ctx.dewpoint_c)) +
          ". ";
  // QNH bleibt nackt (hPa-Ganzzahl); Normalizer-Pass expand_keyword_digits
  // haengt "Hektopascal" idempotent an.
  text += "QNH " + format_qnh(ctx.qnh_inhg) + ". ";
  text += "Bei Erstanruf Information " + std::string(letter_name) + " angeben.";
  return text;
}

std::string generate_atis_text(const xplane_context::XPlaneContext &ctx) {
  if (settings::atc_profile() == "DE")
    return generate_atis_text_de(ctx);

  std::string airport =
      !ctx.nearest_airport_name.empty()
          ? ctx.nearest_airport_name
          : (!ctx.nearest_airport_id.empty() ? ctx.nearest_airport_id
                                             : "Airport");
  const char *letter_name = kLetterNames[letter_ - 'A'];
  std::string eff = atc_state_machine::effective_runway(ctx);
  std::string runway = eff.empty() ? "unknown" : eff;

  std::string text;
  text += airport + " Information " + letter_name + ". ";
  text += "Runway in use " + runway + ". ";
  text +=
      "Wind " + format_wind(ctx.wind_direction_deg, ctx.wind_speed_kt) + ". ";
  text += "Visibility " + format_visibility(ctx.visibility_m) + ". ";
  text += format_clouds(ctx.cloud_type, ctx.cloud_base_ft_msl) + " ";
  text += "Temperature " + std::to_string(static_cast<int>(ctx.temperature_c)) +
          ", dewpoint " + std::to_string(static_cast<int>(ctx.dewpoint_c)) +
          ". ";
  // Non-US regions (EU, ...) use QNH in hPa per ICAO Doc 4444.
  if (settings::atc_profile() == "US")
    text += "Altimeter " + format_altimeter(ctx.qnh_inhg) + ". ";
  else
    text += "QNH " + format_qnh(ctx.qnh_inhg) + ". ";
  text += "Advise on initial contact you have information " +
          std::string(letter_name) + ".";

  return text;
}

// Out-of-range check: ~60 NM realistic for ground-level ATIS VHF.
static bool atis_in_range(const xplane_context::XPlaneContext &ctx) {
  if (ctx.airport_lat == 0.0 && ctx.airport_lon == 0.0)
    return true; // unknown airport position — assume in range
  double dlat = (ctx.latitude - ctx.airport_lat) * 60.0; // 1 deg = 60 NM
  double dlon = (ctx.longitude - ctx.airport_lon) * 60.0 *
                std::cos(ctx.latitude * M_PI / 180.0);
  double dist_nm = std::sqrt(dlat * dlat + dlon * dlon);
  return dist_nm <= 60.0;
}

static bool freq_matches_atis(float freq, float atis_freq) {
  // Match within 0.005 MHz tolerance (833 kHz spacing rounding)
  return std::fabs(freq - atis_freq) < 0.005f;
}

int which_com_tuned_to_atis(const xplane_context::XPlaneContext &ctx) {
  if (ctx.atis_freq_mhz < 100.0f)
    return 0;
  if (!atis_in_range(ctx))
    return 0;

  bool com1_match = freq_matches_atis(ctx.com1_freq_mhz, ctx.atis_freq_mhz);
  bool com2_match = freq_matches_atis(ctx.com2_freq_mhz, ctx.atis_freq_mhz);

  if (!com1_match && !com2_match)
    return 0;
  if (com1_match && com2_match)
    return ctx.active_com == 2 ? 2 : 1;
  return com1_match ? 1 : 2;
}

bool is_tuned_to_atis(const xplane_context::XPlaneContext &ctx) {
  return which_com_tuned_to_atis(ctx) != 0;
}

} // namespace atis_generator
