# ─────────────────────────────────────────────────────────────────────────────
# Tests — Catch2 v3 (fetched). Mirror the Rust `#[cfg(test)]` unit tests. Links
# denso_core, so every test reaches the ported logic + the view converter.
# ─────────────────────────────────────────────────────────────────────────────
include(FetchContent)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.2
)
FetchContent_MakeAvailable(Catch2)

add_executable(denso_tests
    test_main.cpp
    test_netsh.cpp
    test_parse.cpp
    test_wifi.cpp
    test_format.cpp
    test_settings.cpp
    test_db.cpp
    test_settings_repo.cpp
    test_network_repo.cpp
    test_nmcli.cpp
    test_reassert.cpp
    test_convert.cpp
    test_camera_repo.cpp
    test_area_points.cpp
    test_rtsp_templates.cpp
    # rtsp_templates lives in the GUI app target but depends only on QtCore
    # (QString), so compile it straight into the tests for unit coverage.
    ${CMAKE_SOURCE_DIR}/src/app/ui/camera/shared/rtsp_templates.cpp
    test_snapshot.cpp
    ${CMAKE_SOURCE_DIR}/src/app/ui/camera/shared/snapshot.cpp
    test_roi_geometry.cpp
    # roi_geometry is GUI-target code but depends only on QtGui/QtCore value
    # types (QPointF/QRectF), so compile it into the tests for unit coverage.
    ${CMAKE_SOURCE_DIR}/src/app/ui/camera/shared/roi_geometry.cpp
    test_grid_layout.cpp
    # grid_layout is GUI-target code but pure int math (no Qt), so compile it in.
    ${CMAKE_SOURCE_DIR}/src/app/ui/camera/grid/grid_layout.cpp
)
target_include_directories(denso_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/app)
target_link_libraries(denso_tests PRIVATE denso_core Catch2::Catch2)
target_include_directories(denso_tests PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(denso_tests PRIVATE ${OpenCV_LIBS} Qt6::Gui)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(denso_tests)
