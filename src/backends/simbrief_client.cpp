/*
 * xp_wellys_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 */

#include "backends/simbrief_client.hpp"
#include "core/logging.hpp"
#include "data/simbrief_ofp.hpp"

#include <curl/curl.h>
#include <json.hpp>

#include <atomic>
#include <cstdio>
#include <string>
#include <thread>

namespace simbrief_client {

namespace {

std::atomic<FetchStatus> g_status{FetchStatus::IDLE};
// g_last_error is only written from the fetch thread before setting status=ERROR,
// and only read from the main thread after that. Access is safe without a mutex
// because the atomic status acts as a release/acquire barrier.
static std::string g_last_error;

size_t write_to_string(char *ptr, size_t size, size_t nmemb, void *userdata) {
  auto *buf = static_cast<std::string *>(userdata);
  buf->append(ptr, size * nmemb);
  return size * nmemb;
}

void do_fetch(int pilot_id) {
  g_status.store(FetchStatus::FETCHING);
  g_last_error.clear();

  char url[256];
  std::snprintf(url, sizeof(url),
                "https://www.simbrief.com/api/xml.fetcher.php?userid=%d&json=1",
                pilot_id);

  CURL *curl = curl_easy_init();
  if (!curl) {
    g_last_error = "curl_easy_init failed";
    g_status.store(FetchStatus::ERROR);
    return;
  }

  std::string body;
  curl_easy_setopt(curl, CURLOPT_URL, url);
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_string);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
  curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 5L);
  curl_easy_setopt(curl, CURLOPT_TIMEOUT, 15L);
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

  CURLcode res = curl_easy_perform(curl);
  curl_easy_cleanup(curl);

  if (res != CURLE_OK) {
    g_last_error = curl_easy_strerror(res);
    g_status.store(FetchStatus::ERROR);
    return;
  }

  try {
    using json = nlohmann::json;
    auto j = json::parse(body);

    // SimBrief returns {"fetch":{"status":"Success",...},...}
    std::string api_status =
        j.value("fetch", json::object()).value("status", std::string{});
    if (api_status != "Success") {
      g_last_error = api_status.empty() ? "unexpected response" : api_status;
      g_status.store(FetchStatus::ERROR);
      return;
    }

    simbrief_ofp::OfpData ofp;
    ofp.origin_icao =
        j.value("origin", json::object()).value("icao_code", std::string{});
    {
      auto dest = j.value("destination", json::object());
      ofp.destination_icao = dest.value("icao_code", std::string{});
      // Short airport name: SimBrief returns "airport_name" e.g. "Nice Cote D'Azur".
      // Keep only the first word/city name for clean TTS delivery.
      // Fallback to "name" in case the field key differs across plan types.
      std::string full = dest.value("airport_name", std::string{});
      if (full.empty()) full = dest.value("name", std::string{});
      if (full.empty()) {
        logging::info("[simbrief] destination name fields absent; dest keys:");
        for (auto &[k, v] : dest.items())
          if (v.is_string())
            logging::info("  %s = %s", k.c_str(), v.get<std::string>().c_str());
      }
      if (!full.empty()) {
        auto sp = full.find_first_of(" /");
        ofp.destination_name = (sp != std::string::npos) ? full.substr(0, sp) : full;
        // Title-case first char
        if (!ofp.destination_name.empty())
          ofp.destination_name[0] = static_cast<char>(
              std::toupper(static_cast<unsigned char>(ofp.destination_name[0])));
      }
    }

    auto gen = j.value("general", json::object());

    // SID name: sid_trans is a string when a SID is filed, or an empty
    // JSON object {} when none. gen.value() returns "" for non-string types.
    // Fallback: extract the first token of the route string — SimBrief puts
    // the SID designator there even when sid_trans is empty (e.g. "LSE2A ...").
    {
      std::string sid;
      if (gen.contains("sid_trans") && gen["sid_trans"].is_string())
        sid = gen["sid_trans"].get<std::string>();
      if (sid.empty() || sid == "NONE") {
        // Try first route token as SID candidate.
        std::string route = gen.value("route", std::string{});
        auto sp = route.find(' ');
        std::string first = (sp != std::string::npos) ? route.substr(0, sp) : route;
        // Heuristic: a SID designator ends with a digit+letter (e.g. "LSE2A",
        // "MOBE2D") and is at most 7 chars. Plain waypoints rarely match this.
        if (first.size() >= 4 && first.size() <= 7) {
          char last = first.back();
          char prev = first[first.size() - 2];
          if (std::isalpha(static_cast<unsigned char>(last)) &&
              std::isdigit(static_cast<unsigned char>(prev)))
            sid = first;
        }
      }
      if (sid != "NONE")
        ofp.sid_name = sid;
    }

    // First FPL fix: the waypoint immediately after the SID in the route string.
    // This is the last fix of the ATC-assigned SID — used by cifp_reader to
    // select the correct SID procedure from the CIFP file for the active runway.
    // Route examples: "ODIK2A ODIKI DCT LFMN" → "ODIKI"
    //                 "LTP2A LTPNO ..." → "LTPNO"
    //                 "ODIKI DCT LFMN" (no SID prefix) → "ODIKI"
    {
      std::string route = gen.value("route", std::string{});
      std::istringstream rss(route);
      std::string tok1, tok2;
      if (rss >> tok1) {
        bool tok1_is_sid =
            (tok1 == ofp.sid_name && !ofp.sid_name.empty()) ||
            (tok1.size() >= 4 && tok1.size() <= 7 &&
             std::isalpha(static_cast<unsigned char>(tok1.back())) &&
             std::isdigit(static_cast<unsigned char>(tok1[tok1.size() - 2])));
        if (tok1_is_sid) {
          if (rss >> tok2)
            ofp.fpl_first_fix = tok2;
        } else {
          ofp.fpl_first_fix = tok1;
        }
      }
    }

    // Cruise altitude: SimBrief uses general.cruise_altitude when set, or
    // general.initial_altitude for simple plans with a single cruise level.
    // Neither field is the ATC departure clearance altitude — we keep
    // initial_alt_ft = 0 so CIFP remains the source for {ifr_initial_altitude}.
    {
      std::string cruise_str = gen.value("cruise_altitude", std::string{});
      if (cruise_str.empty() || cruise_str == "null")
        cruise_str = gen.value("initial_altitude", std::string{});
      if (!cruise_str.empty()) {
        try { ofp.cruise_alt_ft = std::stoi(cruise_str); } catch (...) {}
      }
    }

    // Aircraft registration and type.
    auto ac = j.value("aircraft", json::object());
    if (ac.contains("reg") && ac["reg"].is_string())
      ofp.aircraft_reg  = ac["reg"].get<std::string>();
    if (ac.contains("icao_code") && ac["icao_code"].is_string())
      ofp.aircraft_type = ac["icao_code"].get<std::string>();

    // Scheduled takeoff time (Unix timestamp) — for future slot-time check.
    auto times = j.value("times", json::object());
    if (times.contains("sched_off") && times["sched_off"].is_string()) {
      try { ofp.sched_off = std::stoll(times["sched_off"].get<std::string>()); }
      catch (...) {}
    }

    // Navlog: SimBrief returns navlog.fix as an array of waypoints from
    // origin airport to destination airport. Each entry has ident, via_airway,
    // pos_lat, pos_long, altitude_feet, is_sid_star ("0"/"1").
    // Guard: fix may be a JSON array or a single object (single-leg plans).
    {
      auto nl = j.value("navlog", json::object());
      auto fix_node = nl.value("fix", json{});
      // Normalise to an array so the loop below is uniform.
      if (fix_node.is_object())
        fix_node = json::array({fix_node});
      if (fix_node.is_array()) {
        ofp.navlog.reserve(fix_node.size());
        for (const auto &f : fix_node) {
          if (!f.is_object()) continue;
          simbrief_ofp::NavlogFix fix;
          fix.ident       = f.value("ident",       std::string{});
          fix.via_airway  = f.value("via_airway",  std::string{});
          fix.is_sid_star = (f.value("is_sid_star", std::string{"0"}) == "1");
          try {
            auto lat_s = f.value("pos_lat",         std::string{});
            auto lon_s = f.value("pos_long",        std::string{});
            auto alt_s = f.value("altitude_feet",   std::string{});
            if (!lat_s.empty()) fix.lat    = std::stod(lat_s);
            if (!lon_s.empty()) fix.lon    = std::stod(lon_s);
            if (!alt_s.empty()) fix.alt_ft = std::stoi(alt_s);
          } catch (...) {}
          if (!fix.ident.empty())
            ofp.navlog.push_back(std::move(fix));
        }
      }
    }

    // If the route starts with "DCT" (direct clearance with no named SID exit
    // fix), fpl_first_fix ends up as "DCT" which is useless for CIFP lookup.
    // Fall back to the first non-SID/STAR navlog entry that is not the
    // destination airport itself.
    // SimBrief pseudo-waypoints that must never be used as fpl_first_fix:
    // TOC = Top of Climb, TOD = Top of Descent — flight-planning artifacts,
    // not real navigation fixes, and absent from CIFP.
    auto is_pseudo_fix = [](const std::string &id) {
      return id == "TOC" || id == "TOD";
    };

    if (ofp.fpl_first_fix.empty() || ofp.fpl_first_fix == "DCT" ||
        ofp.fpl_first_fix == ofp.destination_icao ||
        is_pseudo_fix(ofp.fpl_first_fix)) {
      for (const auto &fix : ofp.navlog) {
        if (!fix.is_sid_star && !fix.ident.empty() &&
            fix.ident != ofp.destination_icao &&
            !is_pseudo_fix(fix.ident)) {
          ofp.fpl_first_fix = fix.ident;
          break;
        }
      }
    }

    ofp.valid = !ofp.destination_icao.empty();
    simbrief_ofp::set(ofp);

    logging::info("[simbrief] OFP loaded: %s -> %s (%s)  SID=%s  first_fix=%s  cruise=%dft  reg=%s  type=%s  navlog=%zu fixes",
                  ofp.origin_icao.c_str(), ofp.destination_icao.c_str(),
                  ofp.destination_name.empty() ? "no name" : ofp.destination_name.c_str(),
                  ofp.sid_name.empty() ? "none" : ofp.sid_name.c_str(),
                  ofp.fpl_first_fix.empty() ? "none" : ofp.fpl_first_fix.c_str(),
                  ofp.cruise_alt_ft,
                  ofp.aircraft_reg.empty() ? "?" : ofp.aircraft_reg.c_str(),
                  ofp.aircraft_type.empty() ? "?" : ofp.aircraft_type.c_str(),
                  ofp.navlog.size());
    g_status.store(FetchStatus::SUCCESS);

  } catch (const std::exception &e) {
    g_last_error = std::string("parse error: ") + e.what();
    g_status.store(FetchStatus::ERROR);
  }
}

} // namespace

void fetch_async(int pilot_id) {
  if (pilot_id <= 0)
    return;
  if (g_status.load() == FetchStatus::FETCHING)
    return;
  std::thread(do_fetch, pilot_id).detach();
}

FetchStatus status() { return g_status.load(); }

std::string last_error() { return g_last_error; }

} // namespace simbrief_client
