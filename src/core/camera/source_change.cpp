#include "camera/source_change.h"

#include <QByteArray>
#include <QCryptographicHash>

#include <cstdint>
#include <optional>
#include <string>

namespace denso::camera {

// Declared in the header (the wizard's preview gate needs aspect alone); the
// contract lives there.
bool aspect_changed(const Camera& a, const Camera& b) {
    const bool a_known = a.width != 0 && a.height != 0;
    const bool b_known = b.width != 0 && b.height != 0;
    if (a_known != b_known) {
        return true;
    }
    if (!a_known) {
        return false;  // both unknown
    }
    return static_cast<uint64_t>(a.width) * b.height !=
           static_cast<uint64_t>(b.width) * a.height;
}

bool same_effective_source(const Camera& a, const Camera& b) {
    if (a.camera_type != b.camera_type) {
        return false;
    }
    if (a.camera_type == "usb") {
        return a.index == b.index;
    }
    // IP (or any non-USB): the fields that determine which stream/view is opened.
    // Credentials (username/password) are excluded — they don't change the view.
    return a.ip == b.ip && a.rtsp == b.rtsp && a.manufacturer == b.manufacturer &&
           a.channel == b.channel && a.stream == b.stream;
}

bool view_geometry_changed(const Camera& a, const Camera& b) {
    return a.rotation != b.rotation || a.pitch != b.pitch || a.roll != b.roll ||
           aspect_changed(a, b);
}

bool requires_area_review(const Camera& before, const Camera& after) {
    return !same_effective_source(before, after) ||
           view_geometry_changed(before, after);
}

namespace {

// Length-prefixed field encoding. Without it, two different field splits could
// compose the same byte string ("ab" + "c" vs "a" + "bc") and two genuinely
// different views would fingerprint identically — and `rtsp` is free-form
// operator text, so no separator character is safe to reserve.
void put(QByteArray& out, const std::string& s) {
    out += QByteArray::number(static_cast<qlonglong>(s.size()));
    out += ':';
    out += QByteArray::fromStdString(s);
    out += ';';
}

/// An absent optional is its OWN token, never the empty string: "no RTSP URL set"
/// and "the empty RTSP URL" are different configurations and must not collide.
void put(QByteArray& out, const std::optional<std::string>& s) {
    if (!s.has_value()) {
        out += "-;";
        return;
    }
    put(out, *s);
}

void put(QByteArray& out, const std::optional<uint32_t>& v) {
    out += v.has_value() ? QByteArray::number(static_cast<qulonglong>(*v))
                         : QByteArray("-");
    out += ';';
}

/// The REDUCED aspect ratio, so a same-aspect resolution change fingerprints
/// identically — matching aspect_changed, which ignores it because a normalized
/// polygon or line does not move. Either dimension unset means "unknown", the
/// same single token aspect_changed treats as one state.
QByteArray aspect_token(uint32_t w, uint32_t h) {
    if (w == 0 || h == 0) {
        return QByteArray("?");
    }
    uint32_t a = w;
    uint32_t b = h;
    while (b != 0) {
        const uint32_t t = a % b;
        a = b;
        b = t;
    }
    return QByteArray::number(static_cast<qulonglong>(w / a)) + 'x' +
           QByteArray::number(static_cast<qulonglong>(h / a));
}

}  // namespace

std::string view_revision(const Camera& c) {
    // Versioned prefix: if the set of view-significant fields ever changes, every
    // existing fingerprint must stop matching rather than silently keep matching
    // on the fields that happen to have survived.
    QByteArray in("denso.view.v1;");
    put(in, c.camera_type);
    if (c.camera_type == "usb") {
        put(in, c.index);
    } else {
        put(in, c.ip);
        put(in, c.rtsp);
        put(in, c.manufacturer);
        put(in, c.channel);
        put(in, c.stream);
    }
    in += aspect_token(c.width, c.height);
    in += ';';
    in += QByteArray::number(static_cast<qulonglong>(c.rotation));
    in += ';';
    // 'g' with 9 significant digits round-trips a float exactly, so two distinct
    // stored angles can never fingerprint the same.
    in += QByteArray::number(static_cast<double>(c.pitch), 'g', 9);
    in += ';';
    in += QByteArray::number(static_cast<double>(c.roll), 'g', 9);
    in += ';';
    return QCryptographicHash::hash(in, QCryptographicHash::Sha256)
        .toHex()
        .toStdString();
}

} // namespace denso::camera
