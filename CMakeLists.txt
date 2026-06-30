cmake_minimum_required(VERSION 3.21)
project(DensoDigitalReader LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The Qt Widgets UI uses Q_OBJECT (signals/slots) — let CMake run moc.
set(CMAKE_AUTOMOC ON)

# The UI sources carry UTF-8 literals (✕, …, —, 🔒, ×) that must reach the
# binary byte-for-byte. They are saved UTF-8 (no BOM), so tell MSVC to read —
# and emit — UTF-8 rather than the system code page.
add_compile_options($<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

# ─────────────────────────────────────────────────────────────────────────────
# Core library — the parsing/formatting/builder logic ported 1:1 from the Rust
# crate, plus the SQLite persistence layer (db + repos) built on Qt6::Sql. The
# OS network backends and the Qt Widgets UI are layered on top in later commits
# of the port. The pure-std logic and the Qt-backed persistence share one lib
# at this app's scale.
# ─────────────────────────────────────────────────────────────────────────────
find_package(Qt6 REQUIRED COMPONENTS Core Gui Sql Widgets)

add_library(denso_core STATIC
    src/network/netsh.cpp
    src/network/parse.cpp
    src/network/wifi.cpp
    src/network/nmcli.cpp
    src/network/backend.cpp
    src/hardware/format.cpp
    src/hardware/collect.cpp
    src/settings/settings.cpp
    src/db/db.cpp
    src/settings/repo.cpp
    src/network/repo.cpp
    src/ui/convert.cpp
)
target_include_directories(denso_core PUBLIC src)
target_link_libraries(denso_core PUBLIC Qt6::Core Qt6::Sql)

# OS network backend — exactly one platform implementation is compiled; the
# other make_*_backend() declaration stays undefined (never odr-used).
if(WIN32)
    target_sources(denso_core PRIVATE src/network/windows_backend.cpp)
elseif(UNIX AND NOT APPLE)
    target_sources(denso_core PRIVATE src/network/linux_backend.cpp)
endif()

# ─────────────────────────────────────────────────────────────────────────────
# GUI application — the Qt Widgets port of the Slint screens (theme, root
# window, settings/camera modals) + the entry-point orchestrator. WIN32 makes
# it a windowed app (no console) on Windows. APP_VERSION mirrors Rust's
# env!("CARGO_PKG_VERSION").
# ─────────────────────────────────────────────────────────────────────────────
add_executable(denso WIN32
    src/main.cpp
    src/ui/theme.cpp
    src/ui/netcard.cpp
    src/ui/settings_dialog.cpp
    src/ui/camera_dialog.cpp
    src/ui/mainwindow.cpp
)
target_link_libraries(denso PRIVATE denso_core Qt6::Widgets)
target_compile_definitions(denso PRIVATE APP_VERSION="0.1.0")

# ─────────────────────────────────────────────────────────────────────────────
# Tests — Catch2 v3 (fetched). Mirror the Rust `#[cfg(test)]` unit tests.
# ─────────────────────────────────────────────────────────────────────────────
include(FetchContent)
FetchContent_Declare(
    Catch2
    GIT_REPOSITORY https://github.com/catchorg/Catch2.git
    GIT_TAG        v3.5.2
)
FetchContent_MakeAvailable(Catch2)

enable_testing()
add_executable(denso_tests
    tests/test_netsh.cpp
    tests/test_parse.cpp
    tests/test_wifi.cpp
    tests/test_format.cpp
    tests/test_settings.cpp
    tests/test_db.cpp
    tests/test_settings_repo.cpp
    tests/test_network_repo.cpp
    tests/test_nmcli.cpp
    tests/test_reassert.cpp
    tests/test_convert.cpp
)
target_link_libraries(denso_tests PRIVATE denso_core Catch2::Catch2WithMain)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(CTest)
include(Catch)
catch_discover_tests(denso_tests)
