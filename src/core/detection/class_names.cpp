#include "detection/class_names.h"

#include <QByteArray>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonValue>
#include <QString>

namespace denso::detection {

std::string serialize_class_names(const std::vector<std::string>& names) {
    QJsonArray arr;
    for (const std::string& n : names) {
        arr.append(QString::fromStdString(n));
    }
    return QString::fromUtf8(QJsonDocument(arr).toJson(QJsonDocument::Compact))
        .toStdString();
}

std::vector<std::string> parse_class_names(const std::string& text) {
    std::vector<std::string> out;
    const QJsonDocument doc =
        QJsonDocument::fromJson(QByteArray::fromStdString(text));
    if (!doc.isArray()) {
        return out;
    }
    const QJsonArray arr = doc.array();
    out.reserve(static_cast<size_t>(arr.size()));
    for (const QJsonValue& v : arr) {
        out.push_back(v.toString().toStdString());
    }
    return out;
}

} // namespace denso::detection
