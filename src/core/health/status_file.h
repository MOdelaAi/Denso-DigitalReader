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
#include <vector>

namespace denso::health {

/// One zone-level inhibit ONSET: the moment a zone stopped reading, not the
/// standing condition (that is `inhibited_zones`). It needs a record of its own
/// because an escalated zone that then goes silent is dropped from the runtime
/// projection while its alarm is still owed, so a state-derived array alone
/// would lose it.
///
/// Both identifiers are carried: a zone number is only unique machine-wide, and
/// a corrupt/legacy config can have two cameras claim one number. `reason` is a
/// stable STRING code exactly like ZoneIssue's — a FILE FORMAT: never rename,
/// never reuse, only add. A record holds two integers and that token, so no
/// camera URL or credential can travel this path. Deliberately says nothing
/// about backend delivery or acknowledgement; it reports what the appliance
/// observed locally.
struct ZoneInhibitRecord {
    int64_t camera_id = 0;
    int     zone_no   = 0;
    QString reason;
};

// `mode` / `mode_setup_required` are OPTIONAL: emitted only when set, so a
// DB-stage writer that cannot read the mode simply omits both rather than
// guessing (spec §9). Passed as a serialized QString token, not a TargetMode,
// so denso_core/health keeps no dependency on the mode domain.
//
// `zone_inhibit_onsets` is an ADDITIVE trailing parameter: the `zone_inhibit_onsets`
// key is emitted only when the batch is non-empty, so every existing call site —
// and every historical document — is byte-for-byte unchanged. It is a transient
// event batch replaced by the next write, never an acknowledgement ledger.
bool write_status_file(const QString& path,
                       const health::IntegrityVerdict& verdict,
                       const std::map<int64_t, uint32_t>& camera_causes,
                       const std::set<int>& held_zones,
                       const std::set<int>& inhibited_zones,
                       const std::optional<QString>& mode = std::nullopt,
                       std::optional<bool> mode_setup_required = std::nullopt,
                       const std::vector<ZoneInhibitRecord>& zone_inhibit_onsets = {});

} // namespace denso::health
