#include "models/hashing.h"
#include <QCryptographicHash>
#include <QFile>
namespace denso::models {
std::optional<std::string> file_sha256(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return std::nullopt;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    if (!hash.addData(&f)) return std::nullopt;     // Qt6: addData(QIODevice*) -> bool
    return hash.result().toHex().toStdString();
}
}
