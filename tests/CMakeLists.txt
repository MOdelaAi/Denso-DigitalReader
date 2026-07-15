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
    test_detection_repo.cpp
    test_reading_repo.cpp
    test_brazing_config.cpp
    test_area_points.cpp
    test_area_geometry.cpp
    test_class_names.cpp
    test_rtsp_templates.cpp
    # rtsp_templates lives in the GUI app target but depends only on QtCore
    # (QString), so compile it straight into the tests for unit coverage.
    ${CMAKE_SOURCE_DIR}/src/app/ui/camera/shared/rtsp_templates.cpp
    test_gst_pipeline.cpp
    # gst_pipeline is GUI-target code but a pure std::string builder (no Qt/
    # OpenCV), so compile it into the tests for unit coverage.
    ${CMAKE_SOURCE_DIR}/src/app/ui/camera/shared/gst_pipeline.cpp
    test_snapshot.cpp
    ${CMAKE_SOURCE_DIR}/src/app/ui/camera/shared/snapshot.cpp
    test_roi_geometry.cpp
    # roi_geometry is GUI-target code but depends only on QtGui/QtCore value
    # types (QPointF/QRectF), so compile it into the tests for unit coverage.
    ${CMAKE_SOURCE_DIR}/src/app/ui/camera/shared/roi_geometry.cpp
    test_grid_layout.cpp
    # grid_layout is GUI-target code but pure int math (no Qt), so compile it in.
    ${CMAKE_SOURCE_DIR}/src/app/ui/camera/grid/grid_layout.cpp
    test_fps_meter.cpp
    # fps_meter is GUI-target code but pure std::chrono (no Qt), so compile it in.
    ${CMAKE_SOURCE_DIR}/src/app/ui/camera/grid/fps_meter.cpp
    test_letterbox.cpp
    # letterbox is GUI-target code but pure OpenCV (no Qt), so compile it in.
    ${CMAKE_SOURCE_DIR}/src/app/ui/camera/shared/detection/letterbox.cpp
    test_yolo_decode.cpp
    # yolo_decode is GUI-target code but pure OpenCV (no Qt), so compile it in.
    ${CMAKE_SOURCE_DIR}/src/app/ui/camera/shared/detection/yolo_decode.cpp
    test_merge_detections.cpp
    # merge_detections is GUI-target code but pure OpenCV (no Qt), so compile it in.
    ${CMAKE_SOURCE_DIR}/src/app/ui/camera/shared/detection/merge_detections.cpp
    test_names_metadata.cpp
    # names_metadata is GUI-target code but OpenCV/ORT-free pure string parsing,
    # so compile it in.
    ${CMAKE_SOURCE_DIR}/src/app/ui/camera/shared/detection/names_metadata.cpp
    test_frame_convert.cpp
    # frame_convert (qimage_to_mat) is GUI-target code but pure OpenCV/QImage
    # conversion (no widgets), so compile it in.
    ${CMAKE_SOURCE_DIR}/src/app/ui/camera/shared/frame_convert.cpp
    test_stream_pacing.cpp
    # stream_pacing is GUI-target code but pure int math (no Qt/OpenCV), so
    # compile it in.
    ${CMAKE_SOURCE_DIR}/src/app/ui/camera/grid/stream_pacing.cpp
    test_safe_process.cpp
    test_frame_processor.cpp
    # frame_processor is GUI-target code but its link deps (snapshot,
    # frame_convert, zone_assembly, merge_detections, area_geometry) are all
    # already compiled/linked here and inference is behind the InferenceEngine
    # interface (fake-injectable), so compile it in for the worker-firewall test.
    ${CMAKE_SOURCE_DIR}/src/app/ui/camera/grid/frame_processor.cpp
    test_zone_assembly.cpp
    # zone_assembly is GUI-target code but pure (OpenCV value types + core
    # geometry, no widgets/Qt-net), so compile it in.
    ${CMAKE_SOURCE_DIR}/src/app/ui/camera/grid/zone_assembly.cpp
    test_zone_aggregator.cpp
    # zone_aggregator is pure std (no Qt/OpenCV), so compile it in.
    ${CMAKE_SOURCE_DIR}/src/app/ui/camera/grid/zone_aggregator.cpp
    test_brazing_payload.cpp
    # brazing_payload is pure std::string (no Qt/OpenCV), so compile it in.
    ${CMAKE_SOURCE_DIR}/src/app/ui/camera/grid/brazing_payload.cpp
    test_brazing_retry_policy.cpp
    # brazing_retry_policy is GUI-target code but pure std (no Qt/OpenCV), so
    # compile it in.
    ${CMAKE_SOURCE_DIR}/src/app/ui/camera/grid/brazing_retry_policy.cpp
    test_warmup_gate.cpp
    # warmup_gate is GUI-target code but pure std (no Qt/OpenCV), so compile it in.
    ${CMAKE_SOURCE_DIR}/src/app/ui/camera/grid/warmup_gate.cpp
    test_startup_mode.cpp
    # startup_mode is GUI-target code but pure std::filesystem (no Qt/OpenCV), so
    # compile it in.
    ${CMAKE_SOURCE_DIR}/src/app/ui/startup_mode.cpp
    test_class_names_sidecar.cpp
    # class_names_sidecar reads a JSON sidecar via Qt Core (no widgets), so
    # compile it in for unit coverage.
    ${CMAKE_SOURCE_DIR}/src/app/ui/camera/shared/detection/class_names_sidecar.cpp
    test_engine_requirements.cpp
    # engine_requirements is a header-only pure helper (no backend), so the test
    # needs no extra source.
)
target_include_directories(denso_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/app)
target_link_libraries(denso_tests PRIVATE denso_core Catch2::Catch2)
target_include_directories(denso_tests PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(denso_tests PRIVATE ${OpenCV_LIBS} Qt6::Gui)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(denso_tests)
