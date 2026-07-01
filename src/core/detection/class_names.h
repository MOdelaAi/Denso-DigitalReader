// Serialize a model's class-name list to / from the single TEXT column
// `model.class_names`. Stored as a JSON array so names containing commas,
// spaces, or other delimiters round-trip safely. Parsing is tolerant: blank or
// malformed text yields an empty list rather than throwing.
#pragma once

#include <string>
#include <vector>

namespace denso::detection {

std::string serialize_class_names(const std::vector<std::string>& names);
std::vector<std::string> parse_class_names(const std::string& text);

} // namespace denso::detection
