cmake_minimum_required(VERSION 3.21)
project(DensoDigitalReader LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ─────────────────────────────────────────────────────────────────────────────
# Core library — the parsing/formatting/builder logic ported 1:1 from the Rust
# crate, plus the SQLite persistence layer (db + repos) built on Qt6::Sql. The
# OS network backends and the Qt Widgets UI are layered on top in later commits
# of the port. The pure-std logic and the Qt-backed persistence share one lib
# at this app's scale.
# ─────────────────────────────────────────────────────────────────────────────
find_package(Qt6 REQUIRED COMPONENTS Core Sql)

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
)
target_link_libraries(denso_tests PRIVATE denso_core Catch2::Catch2WithMain)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(CTest)
include(Catch)
catch_discover_tests(denso_tests)
