# ─────────────────────────────────────────────────────────────────────────────
# denso — the Qt Widgets port of the Slint screens (theme, root window,
# settings/camera modals) + the entry-point orchestrator. WIN32 makes it a
# windowed app (no console) on Windows. APP_VERSION mirrors the Rust
# env!("CARGO_PKG_VERSION"). This directory is the app's include root, so the
# widget headers read `ui/theme.h` etc.; core headers come via denso_core.
# ─────────────────────────────────────────────────────────────────────────────
add_executable(denso WIN32
    main.cpp
    ui/theme.cpp
    ui/camera_view.cpp
    ui/camera_devices.cpp
    ui/netcard.cpp
    ui/settings_dialog.cpp
    ui/camera_dialog.cpp
    ui/mainwindow.cpp
)
target_include_directories(denso PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(denso PRIVATE denso_core Qt6::Widgets Qt6::Multimedia)
target_compile_definitions(denso PRIVATE APP_VERSION="0.1.0")
