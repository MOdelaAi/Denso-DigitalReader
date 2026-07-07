// Pure gating for UI-first startup: which cameras are still waiting on detection
// models to finish warming, and which become startable as each model goes ready.
// No Qt — unit-tested like the other grid policy units. The CameraGrid holds the
// per-camera Qt/data and calls in here to decide when to start a stream.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace denso::ui {

class PendingStart {
public:
    /// Register a camera that cannot start until every file in
    /// `not_yet_ready_models` has been reported ready. Pass only the models that
    /// are NOT already ready at registration time (a camera with none left to
    /// wait for should be started immediately by the caller, not registered).
    void add(int64_t camera_id, std::vector<std::string> not_yet_ready_models);

    /// Mark one model file ready. Returns the camera_ids that are now fully
    /// satisfied (their last outstanding model just went ready); those cameras
    /// are removed from tracking. Order preserved by registration.
    std::vector<int64_t> ready(const std::string& model);

    /// Remove and return every still-pending camera_id (used on warm-up finish
    /// to start the remainder with a fallback processor).
    std::vector<int64_t> drain();

    bool empty() const { return entries_.empty(); }

private:
    struct Entry {
        int64_t camera_id;
        std::vector<std::string> remaining;  // models still not ready
    };
    std::vector<Entry> entries_;
};

} // namespace denso::ui
