# ─────────────────────────────────────────────────────────────────────────────
# denso_core — the parsing/formatting/builder logic + SQLite persistence ported
# 1:1 from the Rust crate, plus the Qt-free domain↔view boundary (ui/convert +
# ui/viewmodel). Depends only on Qt6::Core/Sql, never Qt6::Widgets, so the GUI
# can't leak into the testable core. Headers are co-located with sources; this
# directory is the public include root, so includes read `network/model.h`,
# `ui/convert.h`, `strutil.h`, etc.
# ─────────────────────────────────────────────────────────────────────────────
add_library(denso_core STATIC
    network/backend.cpp
    network/repo.cpp
    network/windows/netsh.cpp   # pure helpers — compiled on every OS (tested off-device)
    network/windows/parse.cpp
    network/windows/wifi.cpp
    network/linux/nmcli.cpp
    hardware/format.cpp
    hardware/collect.cpp
    settings/settings.cpp
    db/db.cpp
    settings/repo.cpp
    camera/repo.cpp
    camera/area_points.cpp
    camera/area_geometry.cpp
    detection/class_names.cpp
    detection/repo.cpp
    ui/convert.cpp
)
target_include_directories(denso_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(denso_core PUBLIC Qt6::Core Qt6::Sql)

# OS network backend — exactly one platform implementation is compiled; the
# other make_*_backend() declaration stays undefined (never odr-used).
if(WIN32)
    target_sources(denso_core PRIVATE network/windows/windows_backend.cpp)
elseif(UNIX AND NOT APPLE)
    target_sources(denso_core PRIVATE network/linux/linux_backend.cpp)
endif()
