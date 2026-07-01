#include "ui/camera/shared/detection/engine_registry.h"

namespace denso::ui {

InferenceEngine* EngineRegistry::get(const std::string& filename) {
    auto it = engines_.find(filename);
    if (it == engines_.end()) {
        auto eng = std::make_unique<OrtEngine>(models_dir_ + "/" + filename,
                                               cache_dir_);
        it = engines_.emplace(filename, std::move(eng)).first;
    }
    OrtEngine* e = it->second.get();
    return (e && e->ok()) ? e : nullptr;
}

} // namespace denso::ui
