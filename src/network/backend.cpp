#include "network/backend.h"

#include "network/repo.h"

#include <exception>

namespace denso::network {

std::unique_ptr<NetworkBackend> backend() {
#if defined(_WIN32)
    return make_windows_backend();
#elif defined(__linux__)
    return make_linux_backend();
#else
    return std::make_unique<NullBackend>();
#endif
}

std::vector<std::pair<std::string, std::string>> reassert(const QSqlDatabase& db,
                                                          const NetworkBackend& backend) {
    // NOTE: `all()` returns an empty vector on a DB read error (no Result type;
    // an accepted divergence from the Rust repo, carried since slice 2), so a
    // genuine DB failure yields no reasserts here rather than the Rust
    // `("<db>", err)` entry. The reassert tests don't exercise that path.
    std::vector<std::pair<std::string, std::string>> errors;
    for (const auto& c : all(db)) {
        try {
            backend.apply_config(c);
        } catch (const std::exception& e) {
            errors.emplace_back(c.iface, e.what());
        }
    }
    return errors;
}

} // namespace denso::network
