cmake_minimum_required(VERSION 3.21)
project(DensoDigitalReader LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The Qt Widgets UI uses Q_OBJECT (signals/slots) — let CMake run moc. The UI
# sources carry UTF-8 literals (✕, …, —, 🔒, ×) that must reach the binary
# byte-for-byte, so tell MSVC to read/emit UTF-8 (the files have no BOM).
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)
add_compile_options($<$<CXX_COMPILER_ID:MSVC>:/utf-8>)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Sql Widgets Multimedia Network)
find_package(OpenCV REQUIRED)

# Inference backend is platform-split:
#   Windows → ONNX Runtime (GPU build, provisioned into third_party/). MinGW
#     links the C API directly against the MSVC-built DLL (clean C ABI). The GPU
#     provider DLLs (CUDA/TensorRT) are loaded by ORT at runtime, not linked.
#   Linux (Jetson) → native TensorRT + CUDA. Engines are PREBUILT with trtexec
#     on-device (never at runtime); we link only the runtime (no nvonnxparser).
if(WIN32)
    set(ORT_DIR ${CMAKE_SOURCE_DIR}/third_party/onnxruntime)
    add_library(onnxruntime SHARED IMPORTED GLOBAL)
    set_target_properties(onnxruntime PROPERTIES
        IMPORTED_LOCATION ${ORT_DIR}/lib/onnxruntime.dll
        IMPORTED_IMPLIB   ${ORT_DIR}/lib/onnxruntime.dll
        INTERFACE_INCLUDE_DIRECTORIES ${ORT_DIR}/include)
else()
    find_package(CUDAToolkit REQUIRED)              # provides the CUDA::cudart target
    find_library(TRT_NVINFER nvinfer REQUIRED)
    find_path(TRT_INCLUDE_DIR NvInfer.h
        PATHS /usr/include/aarch64-linux-gnu /usr/include)
    if(NOT TRT_INCLUDE_DIR)
        message(FATAL_ERROR "NvInfer.h not found — install libnvinfer-headers-dev")
    endif()
    message(STATUS "TensorRT: ${TRT_NVINFER}  headers: ${TRT_INCLUDE_DIR}")
endif()

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
