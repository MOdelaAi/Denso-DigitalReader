// Entry point for denso_integration_tests: the Qt-Widgets/offscreen integration
// suite. Unlike denso_tests (headless, backend-free), these tests exercise real
// widgets (CameraView/CameraGrid) and therefore need exactly one QApplication for
// the whole target. We force the offscreen QPA platform BEFORE constructing it so
// the suite runs on a headless CI/dev box with no display.
//
// The tests seeded here stay MODEL-LESS (active + setup_complete cameras with no
// attached models), so CameraGrid selects OrientationProcessor and never asks the
// EngineRegistry for an engine — no ORT/TensorRT engine is deserialized or built,
// no GPU inference runs.
#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>

#include <QApplication>

int main(int argc, char** argv) {
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QApplication app(argc, argv);
    return Catch::Session().run(argc, argv);
}

// Harness sanity: proves the target links denso_app, that the single offscreen
// QApplication is live for the whole session, and that catch_discover_tests finds
// a case in this binary. Real lifecycle cases live in their own sources.
TEST_CASE("integration harness has a live offscreen QApplication", "[harness]") {
    REQUIRE(QCoreApplication::instance() != nullptr);
    CHECK(qgetenv("QT_QPA_PLATFORM") == QByteArray("offscreen"));
}
