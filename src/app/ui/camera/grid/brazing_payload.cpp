#include "ui/camera/grid/brazing_payload.h"

namespace denso::ui {

std::string build_brazing_payload(const std::map<int, int>& zones) {
    std::string out = "{";
    bool first = true;
    for (const auto& [zone_no, value] : zones) {
        if (!first) {
            out += ",";
        }
        first = false;
        out += "\"zone";
        out += std::to_string(zone_no);
        out += "\":";
        out += std::to_string(value);
    }
    out += "}";
    return out;
}

} // namespace denso::ui
