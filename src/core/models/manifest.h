#pragma once
#include <optional>
#include <string>
#include <vector>
namespace denso::models {
struct ModelGeneration {
    std::string name, engine, engine_sha256, sidecar, sidecar_sha256;
    std::vector<std::string> class_names;
    std::string trt, cuda, sm, installed_utc, state;
};
struct Manifest { int schema = 0; std::vector<ModelGeneration> generations; };
struct ParseResult { std::optional<Manifest> manifest; std::string error; };
ParseResult parse_manifest(const std::string& json_text);   // structural only
}
