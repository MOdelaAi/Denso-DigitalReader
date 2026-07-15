#include "camera/stream_pacing.h"

#include <algorithm>

namespace denso::ui {

int next_backoff_ms(int current_ms) {
    if (current_ms <= 0) {
        return 1000;
    }
    return std::min(current_ms * 2, 10000);
}

bool should_emit(int in_flight, int max_in_flight) {
    return in_flight < max_in_flight;
}

} // namespace denso::ui
