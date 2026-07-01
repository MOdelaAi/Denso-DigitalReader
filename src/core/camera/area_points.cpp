#include "camera/area_points.h"

#include <QString>
#include <QStringList>

namespace denso::camera {

std::string serialize_points(const std::vector<Point>& points) {
    QStringList pairs;
    pairs.reserve(static_cast<qsizetype>(points.size()));
    for (const Point& p : points) {
        // 'g' with default precision keeps short values short ("0.25") and is
        // locale-independent (always a '.' decimal point), unlike std::to_string.
        pairs << QStringLiteral("%1,%2").arg(QString::number(p.x))
                                        .arg(QString::number(p.y));
    }
    return pairs.join(QLatin1Char(';')).toStdString();
}

std::vector<Point> parse_points(const std::string& text) {
    std::vector<Point> out;
    const QString s = QString::fromStdString(text);
    const QStringList tokens = s.split(QLatin1Char(';'), Qt::SkipEmptyParts);
    for (const QString& tok : tokens) {
        const QStringList xy = tok.split(QLatin1Char(','));
        if (xy.size() != 2) {
            continue;  // not an "x,y" pair — skip
        }
        bool ok_x = false;
        bool ok_y = false;
        const float x = xy[0].trimmed().toFloat(&ok_x);
        const float y = xy[1].trimmed().toFloat(&ok_y);
        if (ok_x && ok_y) {
            out.push_back(Point{x, y});
        }
    }
    return out;
}

} // namespace denso::camera
