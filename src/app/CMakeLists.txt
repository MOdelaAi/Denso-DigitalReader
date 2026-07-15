# ─────────────────────────────────────────────────────────────────────────────
# denso — the Qt Widgets GUI + the entry-point orchestrator. WIN32 makes it a
# windowed app (no console) on Windows. APP_VERSION mirrors the Rust
# env!("CARGO_PKG_VERSION"). This directory is the app's include root, so the
# widget headers read `ui/theme.h`, `ui/camera/camera_dialog.h`, etc.; core
# headers come via denso_core. UI is grouped by feature: the app shell at
# ui/ root, plus ui/settings/ and ui/camera/.
# ─────────────────────────────────────────────────────────────────────────────
# ── denso_detection: the pure inference helpers (letterbox / YOLO decode / NMS
#    merge / ONNX-names metadata / class-names sidecar) with NO inference-backend
#    dependency. Both `denso` and `denso_tests` LINK this instead of compiling the
#    same .cpp twice (the old test/build boundary smell). Backend engines
#    (ort/trt/registry/model_sync) stay in `denso` — they're platform-split.
add_library(denso_detection STATIC
    detection/letterbox.cpp
    detection/yolo_decode.cpp
    detection/merge_detections.cpp
    detection/names_metadata.cpp
    detection/class_names_sidecar.cpp
)
target_include_directories(denso_detection PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR} ${OpenCV_INCLUDE_DIRS})
target_link_libraries(denso_detection PUBLIC denso_core Qt6::Core ${OpenCV_LIBS})

add_executable(denso WIN32
    main.cpp
    resources.qrc
    # logging (bounded rotating file sink + redaction)
    logging/log_rotation.cpp
    logging/log_sink.cpp
    logging/log_throttle.cpp
    logging/redact.cpp
    # app shell
    ui/theme.cpp
    ui/mainwindow.cpp
    ui/startup_screen.cpp
    ui/warmup_worker.cpp
    ui/warmup_state.cpp
    ui/startup_mode.cpp
    ui/startup.cpp
    # common dialog primitives (leaf — used by every dialog)
    ui/common/form_widgets.cpp
    ui/common/dialog_chrome.cpp
    ui/common/async_runner.cpp
    # settings modal
    ui/settings/settings_dialog.cpp
    ui/settings/network_panel.cpp
    ui/settings/netcard.cpp
    # camera management — entry points
    ui/camera/camera_view.cpp
    ui/camera/camera_dialog.cpp
    ui/camera/wizard_controller.cpp
    # camera — cross-cutting primitives (used by both surfaces)
    ui/camera/shared/snapshot.cpp
    ui/camera/shared/frame_convert.cpp
    ui/camera/shared/rtsp_templates.cpp
    ui/camera/shared/gst_pipeline.cpp
    ui/camera/shared/roi_geometry.cpp
    # (detection: pure decode/letterbox/merge/metadata/sidecar are in the
    #  denso_detection lib below; the backend-coupled registry + sync stay here)
    detection/engine_registry.cpp
    detection/model_sync.cpp
    # camera — live-view grid internals
    ui/camera/grid/camera_grid.cpp
    ui/camera/grid/warmup_gate.cpp
    ui/camera/grid/camera_stream.cpp
    ui/camera/grid/stream_pacing.cpp
    ui/camera/grid/zone_assembly.cpp
    ui/camera/grid/zone_aggregator.cpp
    ui/camera/grid/brazing_payload.cpp
    ui/camera/grid/brazing_retry_policy.cpp
    ui/camera/grid/brazing_client.cpp
    ui/camera/grid/brazing_reporter.cpp
    ui/camera/grid/zone_reporter.cpp
    ui/camera/grid/camera_tile.cpp
    ui/camera/grid/frame_processor.cpp
    ui/camera/grid/fps_meter.cpp
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
target_link_libraries(denso PRIVATE denso_core denso_detection Qt6::Widgets Qt6::Multimedia Qt6::Network)
target_include_directories(denso PRIVATE ${OpenCV_INCLUDE_DIRS})
target_link_libraries(denso PRIVATE ${OpenCV_LIBS})
target_compile_definitions(denso PRIVATE APP_VERSION="0.1.0")

# ── Inference backend, platform-split (see top-level CMakeLists.txt) ──────────
if(WIN32)
    # Windows: ONNX Runtime engine + its runtime DLLs beside the exe.
    target_sources(denso PRIVATE detection/ort_engine.cpp)
    target_link_libraries(denso PRIVATE onnxruntime)

    # ORT runtime DLLs + the models must sit beside the exe (Windows DLL search +
    # the app scans models/ relative to the executable dir at startup via
    # sync_models). ALL models/*.onnx are deployed so every model in the source
    # models/ folder appears in the catalog. (Configure-time glob — re-run cmake
    # after adding a new .onnx, or drop it into <exe-dir>/models directly.)
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

    # CUDA execution-provider runtime dependencies (nvinfer/nvonnxparser/cuDNN),
    # provisioned into third_party/gpu_ep/ (git-ignored — see docs/GPU_SETUP.md).
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
else()
    # Linux/Jetson: native TensorRT. Engines are PREBUILT on-device with trtexec
    # and placed in <exe-dir>/models by the operator (not copied at build time).
    target_sources(denso PRIVATE
        detection/trt_engine.cpp)
    target_link_libraries(denso PRIVATE ${TRT_NVINFER} CUDA::cudart)
    target_include_directories(denso PRIVATE ${TRT_INCLUDE_DIR})
endif()
