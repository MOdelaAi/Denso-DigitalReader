cmake_minimum_required(VERSION 3.21)
project(DensoDigitalReader LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The Qt Widgets UI uses Q_OBJECT (signals/slots) — let CMake run moc. The UI
# sources carry UTF-8 literals (✕, …, —, 🔒, ×) that must reach the binary
# byte-for-byte, so tell MSVC to read/emit UTF-8 (the files have no BOM).
set(CMAKE_AUTOMOC ON)
add_compile_options($<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Sql Widgets Multimedia Network)
find_package(OpenCV REQUIRED)

# ONNX Runtime (GPU build, provisioned into third_party/). MinGW links the C
# API directly against the MSVC-built DLL (clean C ABI — verified). The GPU
# provider DLLs (CUDA/TensorRT) are loaded by ORT at runtime, not linked.
set(ORT_DIR ${CMAKE_SOURCE_DIR}/third_party/onnxruntime)
add_library(onnxruntime SHARED IMPORTED GLOBAL)
set_target_properties(onnxruntime PROPERTIES
    IMPORTED_LOCATION ${ORT_DIR}/lib/onnxruntime.dll
    IMPORTED_IMPLIB   ${ORT_DIR}/lib/onnxruntime.dll
    INTERFACE_INCLUDE_DIRECTORIES ${ORT_DIR}/include)

enable_testing()

# Two targets, one per concern (see each subdir's CMakeLists):
#   src/core — denso_core  : ported logic + SQLite persistence + the Qt-free
#                            domain↔view boundary. Qt Core/Sql only — the GUI
#                            cannot leak into this testable core.
#   src/app  — denso       : the Qt Widgets GUI + entry-point orchestrator.
#   tests    — denso_tests : Catch2 unit tests over denso_core.
add_subdirectory(src/core)
add_subdirectory(src/app)
add_subdirectory(tests)
