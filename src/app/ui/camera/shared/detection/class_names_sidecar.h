// Reads class names for a native-TensorRT engine from a sidecar file next to it:
// <engine dir>/<stem>.names.json — a JSON array of strings. TensorRT engines
// carry no class-name metadata, so an engine-only deployment ships this file
// beside each .engine. Returns nullopt when the sidecar is absent; throws
// std::runtime_error on malformed/empty JSON.
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace denso::ui {

std::optional<std::vector<std::string>>
read_names_sidecar(const std::filesystem::path& engine_path);

} // namespace denso::ui
