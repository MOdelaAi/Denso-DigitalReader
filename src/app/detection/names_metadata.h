// Parse the class-name list from a YOLO ONNX model's `names` custom metadata,
// which Ultralytics stores as a Python-dict repr: "{0: '0', 1: '1', ...}". The
// values are extracted in the order they appear (key order). Pure — unit-tested.
#pragma once

#include <string>
#include <vector>

namespace denso::ui {

std::vector<std::string> parse_names_metadata(const std::string& names);

} // namespace denso::ui
