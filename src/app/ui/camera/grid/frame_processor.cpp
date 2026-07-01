#include "ui/camera/grid/frame_processor.h"

#include "ui/camera/shared/snapshot.h"  // apply_orientation

namespace denso::ui {

OrientationProcessor::OrientationProcessor(int degrees, double pitch, double roll)
    : degrees_(degrees), pitch_(pitch), roll_(roll) {}

QImage OrientationProcessor::process(const QImage& frame) {
    return apply_orientation(frame, degrees_, pitch_, roll_);
}

} // namespace denso::ui
