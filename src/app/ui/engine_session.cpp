#include "ui/engine_session.h"

#include "detection/engine_registry.h"
#include "detection/repo.h"
#include "models/compatibility.h"
#include "models/manifest.h"
#include "models/model_identity.h"
#include "paths/paths.h"
#include "platform/platform_info.h"

#include <set>
#include <string>
#include <utility>
#include <vector>

namespace denso::ui {

std::shared_ptr<EngineRegistry> build_engine_registry(const QSqlDatabase& db,
                                                      mode::TargetMode mode) {
    // The production manifest view (the active backend is bound inside it) and
    // the ONE shared measured-platform provider — the same seams boot, --check
    // and CameraGrid use, so every judgement about what is declared and what
    // device this is comes from the same facts.
    //
    // A platform probe failure FAILS CLOSED: an empty PlatformInfo corroborates
    // no built_for, so nothing is authorized. Never a substituted constant.
    const denso::models::ManifestView view =
        denso::models::load_manifest_view(denso::paths::models_dir());
    const denso::models::PlatformInfo platform =
        denso::platform::measured_platform_info();

    std::vector<denso::models::ModelMetadata> metadata;
    for (const auto& row : detection::list_models(db)) {
        metadata.push_back(denso::models::resolve_model_metadata(view, row, platform));
    }
    // The ONLY set warm-up may load: what the central policy permits in THIS
    // mode. Never a union across modes.
    std::set<std::string> allow_list =
        denso::models::loadable_model_files(mode, metadata);

    // The models configured cameras actually need — MODE-FILTERED, so a rejected
    // attachment is never in the fail-loud set. warm_up fails loud only for an
    // ALLOWED model that is missing or invalid.
    std::vector<std::string> required =
        detection::attached_model_filenames(db, mode, view, platform);

    return std::make_shared<EngineRegistry>(
        denso::paths::models_dir().toStdString(),
        denso::paths::trt_cache_dir().toStdString(), std::move(allow_list),
        std::move(required));
}

}  // namespace denso::ui
