cmake_minimum_required(VERSION 3.21)
project(DensoDigitalReader LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ─────────────────────────────────────────────────────────────────────────────
# Logic core — pure std C++, no Qt. Carries the parsing/formatting/builder
# logic ported 1:1 from the Rust crate. The SQLite persistence layer, the OS
# network backends, and the Qt Widgets UI are layered on top of this in later
# commits of the port.
# ─────────────────────────────────────────────────────────────────────────────
add_library(denso_core STATIC
    src/network/netsh.cpp
    src/network/parse.cpp
    src/network/wifi.cpp
    src/hardware/format.cpp
    src/settings/settings.cpp
)
target_include_directories(denso_core PUBLIC src)

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
)
target_link_libraries(denso_tests PRIVATE denso_core Catch2::Catch2WithMain)

list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
include(CTest)
include(Catch)
catch_discover_tests(denso_tests)
