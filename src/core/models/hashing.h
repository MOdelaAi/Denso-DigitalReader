#pragma once
#include <QString>
#include <optional>
#include <string>
namespace denso::models {
std::optional<std::string> file_sha256(const QString& path);
}
