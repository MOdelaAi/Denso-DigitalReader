#include "brazing/brazing_payload.h"

namespace denso::ui {

std::string build_brazing_payload(const std::map<int, ZoneValue>& zones) {
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
        // zone_value_json, NOT std::to_string: the value is fixed-point, and
        // this is the ONE place its decimal point is rendered for the wire. A
        // whole-percent Ball value (dp 0) renders exactly as it always did, so
        // the existing backend contract is unchanged.
        out += zone_value_json(value);
    }
    out += "}";
    return out;
}

} // namespace denso::ui
