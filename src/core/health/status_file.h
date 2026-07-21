// Machine-readable local health, for SSH/denso-setup inspection. Written
// ATOMICALLY (temp + rename): an abrupt restart must never leave a half-written
// file that reads as misleading status (spec §7).
#pragma once

#include "health/integrity.h"

#include <QString>
#include <cstdint>
#include <map>
#include <set>

namespace denso::health {

bool write_status_file(const QString& path,
                       const health::IntegrityVerdict& verdict,
                       const std::map<int64_t, uint32_t>& camera_causes,
                       const std::set<int>& held_zones,
                       const std::set<int>& inhibited_zones);

} // namespace denso::health
