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
    # camera management — entry points
    ui/camera/camera_view.cpp
    ui/camera/camera_dialog.cpp
    # camera — cross-cutting primitives (used by both surfaces)
    ui/camera/shared/snapshot.cpp
    ui/camera/shared/frame_convert.cpp
    ui/camera/shared/rtsp_templates.cpp
    ui/camera/shared/roi_geometry.cpp
    ui/camera/shared/detection/letterbox.cpp
    ui/camera/shared/detection/yolo_decode.cpp
    ui/camera/shared/detection/names_metadata.cpp
    ui/camera/shared/detection/ort_engine.cpp
    ui/camera/shared/detection/engine_registry.cpp
    ui/camera/shared/detection/model_sync.cpp
    # camera — live-view grid internals
    ui/camera/grid/camera_grid.cpp
    ui/camera/grid/camera_stream.cpp
    ui/camera/grid/camera_tile.cpp
    ui/camera/grid/frame_processor.cpp
    ui/camera/grid/grid_layout.cpp
    # camera — management modal internals
    ui/camera/dialog/page_util.cpp
    ui/camera/dialog/list_page.cpp
    ui/camera/dialog/add_page.cpp
    ui/camera/dialog/configure_page.cpp
    ui/camera/dialog/models_page.cpp
    ui/camera/dialog/areas_page.cpp
    ui/camera/dialog/wizard_stepper.cpp
    ui/camera/dialog/roi_canvas.cpp
    ui/camera/dialog/camera_devices.cpp
    ui/camera/dialog/ip_scan.cpp
)
target_include_directories(denso PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(denso PRIVATE denso_core Qt6::Widgets Qt6::Multimedia Qt6::Network)
target_include_directories(denso PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(denso PRIVATE ${OpenCV_LIBS})
target_compile_definitions(denso PRIVATE APP_VERSION="0.1.0")

target_link_libraries(denso PRIVATE onnxruntime)

# ORT runtime DLLs + the models must sit beside the exe (Windows DLL search +
# the app scans models/ relative to the executable dir at startup via
# sync_models). ALL models/*.onnx are deployed, not just denso.onnx, so every
# model in the source models/ folder appears in the catalog. (Configure-time
# glob — re-run cmake after adding a new .onnx, or drop it into
# <exe-dir>/models directly.)
file(GLOB MODEL_ONNX_FILES "${CMAKE_SOURCE_DIR}/models/*.onnx")
add_custom_command(TARGET denso POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${ORT_DIR}/lib/onnxruntime.dll
        ${ORT_DIR}/lib/onnxruntime_providers_shared.dll
        ${ORT_DIR}/lib/onnxruntime_providers_cuda.dll
        ${ORT_DIR}/lib/onnxruntime_providers_tensorrt.dll
        $<TARGET_FILE_DIR:denso>
    COMMAND ${CMAKE_COMMAND} -E make_directory $<TARGET_FILE_DIR:denso>/models
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${MODEL_ONNX_FILES}
        $<TARGET_FILE_DIR:denso>/models
    VERBATIM)

# CUDA execution-provider runtime dependencies: ORT's CUDA/TensorRT providers
# load TensorRT (nvinfer_*_10.dll, nvonnxparser_10.dll) and cuDNN (cudnn*64_9.dll)
# at runtime, so those DLLs must also sit beside the exe. They're provisioned
# into third_party/gpu_ep/ (git-ignored — see docs/GPU_SETUP.md), NOT auto-found
# on PATH. cudart/cublas come from the CUDA 12 toolkit, which must be installed
# and on PATH. Without this dir the app still runs — it falls back to the CPU
# execution provider — so the copy is a no-op-friendly glob rather than required.
file(GLOB GPU_EP_DLLS "${CMAKE_SOURCE_DIR}/third_party/gpu_ep/*.dll")
if(GPU_EP_DLLS)
    add_custom_command(TARGET denso POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${GPU_EP_DLLS}
            $<TARGET_FILE_DIR:denso>
        VERBATIM)
else()
    message(STATUS "third_party/gpu_ep/ empty — denso will run detection on CPU "
                   "(see docs/GPU_SETUP.md to enable TensorRT/CUDA)")
endif()
