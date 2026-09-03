#include "camera/roi_enhancement.h"

#include <algorithm>

namespace denso::camera {

RoiEnhancement parse_roi_enhancement(int raw) {
    switch (raw) {
        case 1:
            return RoiEnhancement::Low;
        case 2:
            return RoiEnhancement::Medium;
        case 3:
            return RoiEnhancement::High;
        default:
            // 0, negative, and anything above the range this build knows. An
            // explicit switch rather than a clamp: clamping 7 to 3 would turn a
            // value written by a FUTURE build into the strongest local contrast
            // this one has, which is the single worst way to be wrong here.
            return RoiEnhancement::Off;
    }
}

const char* roi_enhancement_label(RoiEnhancement level) {
    switch (level) {
        case RoiEnhancement::Low:
            return "Low";
        case RoiEnhancement::Medium:
            return "Medium";
        case RoiEnhancement::High:
            return "High";
        case RoiEnhancement::Off:
            break;
    }
    return "Off";
}

ImageEnhancement clamp_enhancement(ImageEnhancement e) {
    e.brightness = std::clamp(e.brightness, kMinBrightness, kMaxBrightness);
    e.contrast = std::clamp(e.contrast, kMinContrast, kMaxContrast);
    e.gamma = std::clamp(e.gamma, kMinGamma, kMaxGamma);
    e.saturation = std::clamp(e.saturation, kMinSaturation, kMaxSaturation);
    return e;
}

ImageEnhancement parse_enhancement(int enabled, int local_contrast, int brightness,
                                   int contrast, int gamma, int saturation) {
    ImageEnhancement e;
    // "Exactly 1", not "non-zero": a corrupt or foreign value must not be able to
    // switch processing on for a camera whose operator never enabled it. Off is
    // also the pipeline every installation had before this feature existed, so an
    // unreadable row degrades to the historical behaviour rather than to a guess.
    e.enabled = (enabled == 1);
    // An enumeration, so an unknown value is not "nearly" a known one.
    e.local_contrast = parse_roi_enhancement(local_contrast);
    // Continuous, so the nearest legal value IS the nearest intent.
    e.brightness = brightness;
    e.contrast = contrast;
    e.gamma = gamma;
    e.saturation = saturation;
    return clamp_enhancement(e);
}

} // namespace denso::camera
