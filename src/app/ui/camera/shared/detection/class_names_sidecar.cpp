#include "ui/camera/shared/detection/class_names_sidecar.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QString>

#include <stdexcept>

namespace denso::ui {

std::optional<std::vector<std::string>>
read_names_sidecar(const std::filesystem::path& engine_path) {
    std::filesystem::path sidecar = engine_path;
    sidecar.replace_extension(".names.json");

    if (!std::filesystem::exists(sidecar)) {
        return std::nullopt;
    }

    QFile file(QString::fromStdString(sidecar.string()));
    if (!file.open(QIODevice::ReadOnly)) {
        throw std::runtime_error(
            "Failed to open class-names sidecar '" + sidecar.string() +
            "': " + file.errorString().toStdString());
    }

    QJsonParseError error;
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll(), &error);
    if (error.error != QJsonParseError::NoError) {
        throw std::runtime_error(
            "Malformed class-names sidecar '" + sidecar.string() +
            "': " + error.errorString().toStdString());
    }
    if (!document.isArray()) {
        throw std::runtime_error(
            "Malformed class-names sidecar '" + sidecar.string() +
            "': root must be a JSON array of strings");
    }

    std::vector<std::string> names;
    const QJsonArray array = document.array();
    names.reserve(static_cast<std::size_t>(array.size()));
    for (qsizetype i = 0; i < array.size(); ++i) {
        if (!array.at(i).isString()) {
            throw std::runtime_error(
                "Malformed class-names sidecar '" + sidecar.string() +
                "': element " + std::to_string(i) + " is not a string");
        }
        names.push_back(array.at(i).toString().toStdString());
    }
    if (names.empty()) {
        throw std::runtime_error(
            "Malformed class-names sidecar '" + sidecar.string() +
            "': names array is empty");
    }

    return names;
}

} // namespace denso::ui
