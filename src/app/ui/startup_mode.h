// Picks the launch UX: a cold start (there are detection models to warm but no
// prebuilt TensorRT engine cached yet → the minutes-long build) shows the
// blocking StartupScreen splash; a warm restart (engine already cached) uses the
// fast UI-first path. Pure std::filesystem — unit-tested. See ui/startup.cpp.
#pragma once

#include <string>

namespace denso::ui {

/// True when launch() should show the blocking splash: models_dir has ≥1 *.onnx
/// AND cache_dir has no *.engine (a lone *.timing file does not count as warm).
bool cold_start_needs_splash(const std::string& models_dir,
                             const std::string& cache_dir);

} // namespace denso::ui
