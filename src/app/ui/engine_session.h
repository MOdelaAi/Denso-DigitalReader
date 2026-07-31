// The ONE construction of a mode's inference session.
//
// An EngineRegistry's allow-list is immutable after construction and get()
// throws for anything outside it, so the registry is bound to exactly one
// operating mode for its whole life. That is the security property, and it is
// kept — but it also means a live mode switch cannot reuse the boot registry: it
// must build a NEW one for the committed destination mode.
//
// Both moments — boot (ui/startup.cpp) and a committed switch
// (ui/mainwindow.cpp) — call the function below, so the allow-list and the
// fail-loud required set can never be derived two different ways for the same
// mode. A second construction site would be a second authority; there is one.
//
// There is NEVER a union allow-list. Each registry sees exactly the models the
// central policy permits in ONE mode, so no wrong-mode engine can reach warm-up
// or inference. We swap the object; we never widen the list.
//
// See spec §3.1 and §9 (the Phase-B lifecycle comparison) for why the process is
// not re-executed instead.
#pragma once

#include "mode/mode.h"

#include <QSqlDatabase>

#include <memory>

namespace denso::ui {

class EngineRegistry;

/// Build the immutable, mode-pure registry for `mode`.
///
/// Resolves every catalogued model for the ACTIVE backend through the production
/// manifest view and the measured platform, reduces to what
/// models::loadable_model_files permits in `mode`, and pairs it with the
/// mode-filtered required set (detection::attached_model_filenames) that makes
/// warm-up fail loud for an allowed-but-missing engine.
///
/// `mode` has NO default: a forgotten call site must fail to compile rather than
/// silently authorize one mode's models in another.
std::shared_ptr<EngineRegistry> build_engine_registry(const QSqlDatabase& db,
                                                      mode::TargetMode mode);

}  // namespace denso::ui
