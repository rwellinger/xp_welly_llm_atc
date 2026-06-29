/*
 * xp_wellys_atc - AI-powered ATC voice communication for X-Plane 12
 * Copyright (C) 2026 thWelly & Claude (Anthropic)
 *
 * Licensed under the GNU GPL-3.0-or-later. See LICENSE.
 *
 * Lifted from spikes/spike_e2e/src/whisper_stt.cpp; the spike validated
 * the parameter set on M-series Metal. The only addition vs. the spike
 * is the optional `airport_context` initial-prompt biasing.
 */

#include "backends/whisper_stt.hpp"

#include "core/logging.hpp"
#include "whisper.h"

#include <algorithm>
#include <cstdio>
#include <thread>

#if defined(__linux__)
#include <dirent.h>
#include <dlfcn.h>
#endif

namespace backends {

namespace {
constexpr const char *kBackendTag = "STT-LOCAL";

#if defined(__linux__)
// Returns free GPU VRAM in bytes, 0 if undetectable (→ CPU fallback).
// AMD:    reads mem_info_vram_free from DRM sysfs (exact free VRAM).
// NVIDIA: dlopen libnvidia-ml.so.1 and calls nvmlDeviceGetMemoryInfo
//         (exact free VRAM without any build-time NVML dependency).
static uint64_t detect_gpu_free_vram_bytes() {
    // --- AMD path ---
    for (int card = 0; card < 8; ++card) {
        char path[128];
        snprintf(path, sizeof(path),
                 "/sys/class/drm/card%d/device/mem_info_vram_free", card);
        FILE *f = fopen(path, "r");
        if (!f)
            continue;
        unsigned long long free_bytes = 0;
        const bool ok = (fscanf(f, "%llu", &free_bytes) == 1);
        fclose(f);
        if (ok && free_bytes > 0) {
            logging::info("[%s] AMD VRAM free: %llu MB (card%d)",
                          kBackendTag,
                          (unsigned long long)(free_bytes / 1024ULL / 1024ULL),
                          card);
            return static_cast<uint64_t>(free_bytes);
        }
    }

    // --- NVIDIA path (dlopen NVML — no build-time dependency) ---
    void *nvml = dlopen("libnvidia-ml.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!nvml)
        nvml = dlopen("libnvidia-ml.so", RTLD_NOW | RTLD_LOCAL);
    if (nvml) {
        // Minimal NVML types — avoids pulling in the NVML SDK headers.
        struct NvmlMemory { unsigned long long total, free, used; };
        using pfnInit   = int (*)(void);
        using pfnGetDev = int (*)(unsigned int, void **);
        using pfnMemInfo = int (*)(void *, NvmlMemory *);
        using pfnShut   = int (*)(void);

        auto nvmlInit = (pfnInit)dlsym(nvml, "nvmlInit_v2");
        if (!nvmlInit)
            nvmlInit = (pfnInit)dlsym(nvml, "nvmlInit");
        auto nvmlGetDev  = (pfnGetDev)dlsym(nvml,  "nvmlDeviceGetHandleByIndex");
        auto nvmlMemInfo = (pfnMemInfo)dlsym(nvml, "nvmlDeviceGetMemoryInfo");
        auto nvmlShut    = (pfnShut)dlsym(nvml,    "nvmlShutdown");

        uint64_t result = 0;
        if (nvmlInit && nvmlGetDev && nvmlMemInfo && nvmlInit() == 0) {
            void *dev = nullptr;
            if (nvmlGetDev(0, &dev) == 0 && dev) {
                NvmlMemory mem = {};
                if (nvmlMemInfo(dev, &mem) == 0) {
                    result = static_cast<uint64_t>(mem.free);
                    logging::info("[%s] NVIDIA VRAM free: %llu MB",
                                  kBackendTag,
                                  (unsigned long long)(mem.free / 1024ULL / 1024ULL));
                }
            }
            if (nvmlShut)
                nvmlShut();
        }
        dlclose(nvml);
        if (result > 0)
            return result;
    }

    logging::info("[%s] GPU VRAM detection failed, defaulting to CPU", kBackendTag);
    return 0;
}
#endif // __linux__
} // namespace

WhisperStt::WhisperStt() = default;

WhisperStt::~WhisperStt() {
  if (ctx_)
    whisper_free(ctx_);
}

bool WhisperStt::open(const std::string &model_path,
                      const std::string &language,
                      int gpu_min_free_vram_gb) {
  whisper_context_params cparams = whisper_context_default_params();

#if defined(__APPLE__)
  cparams.use_gpu = true; // Metal on Apple Silicon
#elif defined(__linux__)
  {
    const uint64_t free_vram  = detect_gpu_free_vram_bytes();
    const uint64_t threshold  =
        static_cast<uint64_t>(gpu_min_free_vram_gb) * 1024ULL * 1024ULL * 1024ULL;
    cparams.use_gpu = (free_vram >= threshold);
    logging::info(
        "[%s] GPU threshold: %d GB free, detected: %llu MB free, use_gpu: %s",
        kBackendTag, gpu_min_free_vram_gb,
        (unsigned long long)(free_vram / 1024ULL / 1024ULL),
        cparams.use_gpu ? "yes" : "no");
  }
#else
  cparams.use_gpu = false;
#endif

  cparams.flash_attn = false;
  use_gpu_ = cparams.use_gpu;

  ctx_ = whisper_init_from_file_with_params(model_path.c_str(), cparams);
  if (!ctx_)
    return false;

  // Half the perf cores, mirroring spike_whisper. whisper.cpp scales
  // sub-linearly above 4 threads on M1.
  const unsigned hw = std::thread::hardware_concurrency();
  n_threads_ =
      static_cast<int>(hw == 0 ? 4u : std::min(8u, std::max(2u, hw / 2)));
  lang_ = language.empty() ? "en" : language;
  return true;
}

std::string WhisperStt::transcribe(const std::vector<float> &pcm_16k_mono,
                                   const std::string &airport_context) {
  if (!ctx_ || pcm_16k_mono.empty())
    return {};

  const char *kBackend = use_gpu_ ? "GPU" : "CPU";
  logging::info("[%s][%s] transcribe %zu PCM samples (whisper.cpp, %s)",
                kBackendTag, lang_.c_str(), pcm_16k_mono.size(), kBackend);

  whisper_full_params wparams =
      whisper_full_default_params(WHISPER_SAMPLING_GREEDY);
  wparams.language = lang_.c_str();
  wparams.translate = false;
  wparams.no_context = true;
  wparams.single_segment = false;
  wparams.print_progress = false;
  wparams.print_realtime = false;
  wparams.print_timestamps = false;
  wparams.print_special = false;
  wparams.n_threads = n_threads_;

  // Optional initial-prompt biasing toward the local airport / facility
  // names. Whisper conditions only on this prompt, no other state — so
  // biasing here cannot leak across consecutive transcriptions.
  if (!airport_context.empty()) {
    wparams.initial_prompt = airport_context.c_str();
  }

  if (whisper_full(ctx_, wparams, pcm_16k_mono.data(),
                   static_cast<int>(pcm_16k_mono.size())) != 0) {
    return {};
  }

  std::string transcript;
  const int n_segments = whisper_full_n_segments(ctx_);
  for (int i = 0; i < n_segments; ++i) {
    const char *seg = whisper_full_get_segment_text(ctx_, i);
    if (seg)
      transcript += seg;
  }

  // whisper.cpp sometimes emits a leading space; trim for cleaner
  // downstream prompts.
  while (!transcript.empty() && transcript.front() == ' ') {
    transcript.erase(transcript.begin());
  }
  return transcript;
}

} // namespace backends
