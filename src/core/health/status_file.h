// Machine-readable local health, for SSH/denso-setup inspection. Written
// ATOMICALLY (temp + rename): an abrupt restart must never leave a half-written
// file that reads as misleading status (spec §7).
#pragma once

#include "health/integrity.h"

#include <QString>
#include <cstdint>
#include <map>
#include <optional>
#include <set>

namespace denso::health {

// `mode` / `mode_setup_required` are OPTIONAL: emitted only when set, so a
// DB-stage writer that cannot read the mode simply omits both rather than
// guessing (spec §9). Passed as a serialized QString token, not a TargetMode,
// so denso_core/health keeps no dependency on the mode domain.
bool write_status_file(const QString& path,
                       const health::IntegrityVerdict& verdict,
                       const std::map<int64_t, uint32_t>& camera_causes,
                       const std::set<int>& held_zones,
                       const std::set<int>& inhibited_zones,
                       const std::optional<QString>& mode = std::nullopt,
                       std::optional<bool> mode_setup_required = std::nullopt);

} // namespace denso::health
