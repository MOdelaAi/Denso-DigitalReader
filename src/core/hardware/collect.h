// Host hardware spec for the System section: OS, device (hostname), RAM, total
// storage — collected fresh each launch, never persisted. Read-only. Ported 1:1
// from Rust `hardware::{model,repo}` (sysinfo → Qt `QSysInfo`/`QStorageInfo` +
// a platform RAM call). Byte sizes are formatted via the existing format_bytes.
#pragma once

#include <string>

namespace denso::hardware {

/// Static hardware description shown in the System section.
struct HardwareSpec {
    std::string os;
    std::string device;
    std::string ram;
    std::string storage;
};

/// Read the host's hardware spec. Live, read-only — there is no persistence
/// here by design.
HardwareSpec collect();

} // namespace denso::hardware
