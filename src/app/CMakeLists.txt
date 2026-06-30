# ─────────────────────────────────────────────────────────────────────────────
# denso — the Qt Widgets GUI + the entry-point orchestrator. WIN32 makes it a
# windowed app (no console) on Windows. APP_VERSION mirrors the Rust
# env!("CARGO_PKG_VERSION"). This directory is the app's include root, so the
# widget headers read `ui/theme.h`, `ui/camera/camera_dialog.h`, etc.; core
# headers come via denso_core. UI is grouped by feature: the app shell at
# ui/ root, plus ui/settings/ and ui/camera/.
# ─────────────────────────────────────────────────────────────────────────────
add_executable(denso WIN32
    main.cpp
    # app shell
    ui/theme.cpp
    ui/mainwindow.cpp
    # settings modal
    ui/settings/settings_dialog.cpp
    ui/settings/netcard.cpp
    # camera management
    ui/camera/camera_view.cpp
    ui/camera/camera_dialog.cpp
    ui/camera/camera_devices.cpp
    ui/camera/ip_scan.cpp
    ui/camera/rtsp_templates.cpp
    ui/camera/snapshot.cpp
)
target_include_directories(denso PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(denso PRIVATE denso_core Qt6::Widgets Qt6::Multimedia Qt6::Network)
target_include_directories(denso PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(denso PRIVATE ${OpenCV_LIBS})
target_compile_definitions(denso PRIVATE APP_VERSION="0.1.0")
