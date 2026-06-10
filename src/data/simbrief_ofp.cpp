/*
 * xp_wellys_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 */

#include "data/simbrief_ofp.hpp"

#include <mutex>

namespace simbrief_ofp {

namespace {
static OfpData  g_ofp;
static std::mutex g_mutex;
} // namespace

void set(const OfpData &ofp) {
  std::lock_guard<std::mutex> lk(g_mutex);
  g_ofp = ofp;
}

OfpData get() {
  std::lock_guard<std::mutex> lk(g_mutex);
  return g_ofp;
}

void clear() {
  std::lock_guard<std::mutex> lk(g_mutex);
  g_ofp = {};
}

} // namespace simbrief_ofp
