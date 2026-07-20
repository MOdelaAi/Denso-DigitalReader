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
    test_display.cpp
    test_db.cpp
    test_paths.cpp
    test_single_instance.cpp
    test_settings_repo.cpp
    test_network_repo.cpp
    test_nmcli.cpp
    test_reassert.cpp
    test_convert.cpp
    test_camera_repo.cpp
    test_source_change.cpp
    test_detection_repo.cpp
    test_reading_repo.cpp
    test_brazing_config.cpp
    test_area_points.cpp
    test_area_geometry.cpp
    test_area_validation.cpp
    test_class_names.cpp
    test_rtsp_templates.cpp
    test_gst_pipeline.cpp
    test_snapshot.cpp
    test_roi_geometry.cpp
    test_grid_layout.cpp
    test_fps_meter.cpp
    test_letterbox.cpp
    test_yolo_decode.cpp
    test_merge_detections.cpp
    test_names_metadata.cpp
    test_frame_convert.cpp
    test_camera_runtime.cpp
    test_preview_gate.cpp
    test_stream_pacing.cpp
    test_safe_process.cpp
    test_frame_processor.cpp
    test_zone_assembly.cpp
    test_zone_aggregator.cpp
    test_brazing_payload.cpp
    test_brazing_retry_policy.cpp
    test_warmup_gate.cpp
    test_startup_mode.cpp
    test_class_names_sidecar.cpp
    test_engine_requirements.cpp
    test_log_rotation.cpp
    test_log_sink.cpp
    test_log_throttle.cpp
    test_cli_args.cpp
    test_hashing.cpp
    test_manifest.cpp
    test_class_map.cpp
    test_migrate.cpp
    test_migrate_coordinator.cpp

    # The app units under test come from the linked subsystem libs
    # (denso_detection / denso_brazing / denso_camera — see the link line below),
    # NOT compiled in a second time here. The few units that don't yet have a lib
    # are compiled in directly:
    ${CMAKE_SOURCE_DIR}/src/app/ui/camera/shared/roi_geometry.cpp  # UI geometry
    ${CMAKE_SOURCE_DIR}/src/app/cli/migrate_coordinator.cpp        # headless migrate coordinator
    ${CMAKE_SOURCE_DIR}/src/app/ui/camera/grid/grid_layout.cpp     # grid layout algo
    ${CMAKE_SOURCE_DIR}/src/app/ui/startup_mode.cpp                # cold-start probe
    ${CMAKE_SOURCE_DIR}/src/app/logging/log_rotation.cpp
    ${CMAKE_SOURCE_DIR}/src/app/logging/redact.cpp
    ${CMAKE_SOURCE_DIR}/src/app/logging/log_sink.cpp
    ${CMAKE_SOURCE_DIR}/src/app/logging/log_throttle.cpp
)
target_include_directories(denso_tests PRIVATE ${CMAKE_SOURCE_DIR}/src/app)
target_link_libraries(denso_tests PRIVATE denso_core denso_detection denso_brazing denso_camera Catch2::Catch2)
target_include_directories(denso_tests PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(denso_tests PRIVATE ${OpenCV_LIBS} Qt6::Gui)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(Catch)
catch_discover_tests(denso_tests)

# The packaging policy harness (pure POSIX sh, no deps) behind the SAME gate as
# the C++ tests. It was 339 lines that only ran when someone remembered to — and
# it covers the decisions that run as ROOT on the appliance (apt-plan guard, GDM
# [daemon] editing, model seeding, the preflight drift check). Deliberately
# dependency-free so it runs unchanged on MSYS2 and on the Jetson.
# FATAL, not a soft skip: a missing bash silently dropping this test would
# recreate the very failure this wiring exists to fix — "green because nobody ran
# it". bash is present in every supported environment here (MSYS2 and the
# Jetson), so its absence is a broken toolchain, not a reason to quietly test less.
find_program(BASH_EXECUTABLE bash REQUIRED)
add_test(NAME packaging_policy
         COMMAND ${BASH_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/packaging/run.sh)
