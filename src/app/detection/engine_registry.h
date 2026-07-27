// One shared inference engine per distinct model file. Cameras that attach the
// same model reuse a single Ort::Session (loaded lazily on first request), so
// N cameras on the same model pay for one load, not N. Owns the engines; hand
// out non-owning pointers (never erased, so the pointers stay valid). get() is
// mutex-guarded: it is called from both the warm-up worker (during warm_up) and
// the GUI thread (starting a camera whose models are ready), so the map must be
// synchronized. infer() holds the raw engine pointer and never touches the
// registry, so there is no per-frame locking.
#pragma once

#include "detection/inference_engine.h"
#ifdef _WIN32
#include "detection/ort_engine.h"
#else
#include "detection/trt_engine.h"
#endif

#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <functional>
#include <vector>

namespace denso::ui {

// The concrete backend engine per platform. Both expose ok() and a
// (path, cache_dir) constructor, so the default factory builds either uniformly.
#ifdef _WIN32
using BackendEngine = OrtEngine;
#else
using BackendEngine = TrtEngine;
#endif

class EngineRegistry {
public:
    /// Builds one engine from (path, cache_dir); returns nullptr on a FAILED load
    /// (the caller treats null as "did not load", exactly as before). Injected so
    /// warm_up()/get() are unit-testable with a counting stub — production passes
    /// nothing and gets the default backend factory (OrtEngine / TrtEngine + ok()).
    using EngineFactory = std::function<std::unique_ptr<InferenceEngine>(
        const std::string& path, const std::string& cache_dir)>;

    /// `allow_list` is the set of active-backend model filenames the central
    /// compatibility policy permits in the committed mode
    /// (denso::models::loadable_model_files). It is the ONLY set warm-up may load:
    /// the directory scan SKIPS every file not in it, so a rejected model is never
    /// deserialized. get() called for a filename OUTSIDE the allow-list is a
    /// programming error and throws — every runtime caller resolves through the
    /// same policy, so reaching get() with a rejected filename means the policy was
    /// bypassed. Duplicates are deduplicated (std::set); an empty allow-list loads
    /// nothing. There is NO default: a forgotten caller must fail to compile, not
    /// silently authorize (or silently load nothing by omission).
    ///
    /// `required` is the MODE-FILTERED set of model filenames configured cameras
    /// need (detection::attached_model_filenames). warm_up() fails loud if any is
    /// missing or fails to load, honoring the engine-only/no-fallback contract. A
    /// rejected attachment is excluded upstream, so it can never trigger the abort.
    ///
    /// `factory` defaults to the production backend factory; tests inject a stub.
    EngineRegistry(std::string models_dir, std::string cache_dir,
                   std::set<std::string> allow_list,
                   std::vector<std::string> required = {},
                   EngineFactory factory = {});

    /// Engine for `filename` (resolved under models_dir), or nullptr if it
    /// failed to load. Cached across calls. THROWS std::logic_error if `filename`
    /// is not in the allow-list (a programming error — see the ctor).
    InferenceEngine* get(const std::string& filename);

    /// Load AND warm (one blank inference) every ALLOW-LISTED *.onnx (Windows) /
    /// *.engine (Jetson) in models_dir, on the warm-up worker thread. Files not in
    /// the allow-list are skipped (never get()-ed, never inferred), with at most
    /// one redaction-safe informational line each. `on_model(filename)` fires just
    /// before each model is prepared (progress display); `on_ready(filename)` fires
    /// just after it is successfully loaded + warmed (so the UI can start cameras
    /// that use it). A model that fails to load fires neither on_ready nor a start.
    /// Blocking.
    void warm_up(std::function<void(const std::string&)> on_model = {},
                 std::function<void(const std::string&)> on_ready = {});

private:
    std::string models_dir_;
    std::string cache_dir_;
    std::set<std::string> allow_list_;   // active-backend filenames the policy allows
    std::vector<std::string> required_;  // model files configured cameras need
    EngineFactory factory_;
    std::map<std::string, std::unique_ptr<InferenceEngine>> engines_;
    std::mutex mutex_;
};

} // namespace denso::ui
