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

# ── denso_brazing: the pure, testable reporting logic (combined-zone payload
#    builder, single-flight retry policy, machine-wide zone aggregator, and the
#    ZoneSink impl zone_reporter) with NO Qt-Network dependency. Both `denso` and
#    `denso_tests` LINK it. zone_reporter has zero Qt deps (chrono/mutex/map/set/
#    functional only), so it belongs here — and denso_tests links this lib but not
#    the denso exe, so it must live here for the reporter tests to link. The
#    Qt-Network transport (brazing_client/reporter) stays in `denso`.
add_library(denso_brazing STATIC
    brazing/brazing_payload.cpp
    brazing/brazing_retry_policy.cpp
    brazing/zone_aggregator.cpp
    brazing/zone_reporter.cpp
)
target_include_directories(denso_brazing PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(denso_brazing PUBLIC denso_core Qt6::Core)

# ── denso_camera: non-widget capture + frame-processing infrastructure (frame
#    conversion, GStreamer/RTSP pipeline builders, snapshot, FPS/pacing/warmup
#    helpers, the frame-processing seam + zone assembly). No Qt Widgets. Both
#    `denso` and `denso_tests` LINK it. The capture worker (camera_stream, QObject)
#    stays in `denso`; the grid/dialog WIDGETS stay under ui/.
add_library(denso_camera STATIC
    camera/frame_convert.cpp
    camera/gst_pipeline.cpp
    camera/rtsp_templates.cpp
    camera/snapshot.cpp
    camera/fps_meter.cpp
    camera/preview_gate.cpp
    camera/stream_pacing.cpp
    camera/warmup_gate.cpp
    camera/zone_assembly.cpp
    camera/frame_processor.cpp
)
target_include_directories(denso_camera PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR} ${OpenCV_INCLUDE_DIRS})
target_link_libraries(denso_camera PUBLIC
    denso_core denso_detection Qt6::Core Qt6::Gui ${OpenCV_LIBS})

# ── denso_app: EVERY denso-executable source EXCEPT main.cpp and resources.qrc,
#    compiled once into an OBJECT library so both the shipped `denso` executable
#    and the `denso_integration_tests` target (defined in tests/, after Catch2 is
#    fetched) link the SAME objects. An OBJECT library — NOT STATIC — because
#    object libraries link ALL their objects into every consumer, so no moc or
#    static-initialiser object can be silently culled by static-archive extraction
#    (the STATIC-lib hazard). resources.qrc stays DIRECTLY on `denso` so :/icon.png
#    and the theme resources auto-register into the shipped exe; all POST_BUILD
#    steps (ORT DLL / model / GPU-EP copy) also stay on `denso`.
add_library(denso_app OBJECT
    cli/run_headless.cpp
    cli/migrate_coordinator.cpp
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
    ui/settings/display_confirm_dialog.cpp
    ui/settings/mode_confirm_text.cpp    # pure copy builder (also compiled into denso_tests)
    ui/settings/mode_confirm_dialog.cpp
    ui/settings/network_panel.cpp
    ui/settings/netcard.cpp
    # camera management — entry points
    ui/camera/camera_view.cpp
    ui/camera/camera_dialog.cpp
    ui/camera/wizard_controller.cpp
    # camera runtime: the capture worker (QObject). The rest of the non-widget
    # capture/frame-processing infra lives in the denso_camera lib below.
    camera/camera_stream.cpp
    ui/camera/shared/roi_geometry.cpp   # widget↔normalized ROI mapping (UI)
    # (detection: pure decode/letterbox/merge/metadata/sidecar are in the
    #  denso_detection lib below; the backend-coupled registry + sync stay here)
    detection/engine_registry.cpp
    detection/model_sync.cpp
    # camera — live-view grid WIDGETS
    ui/camera/grid/camera_grid.cpp
    ui/camera/grid/camera_tile.cpp
    ui/camera/grid/grid_layout.cpp
    # brazing/reporting: the Qt-Network transport only (pure logic —
    # payload/retry_policy/aggregator/zone_reporter — is in the denso_brazing lib)
    brazing/brazing_client.cpp
    brazing/brazing_reporter.cpp
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
# PUBLIC so every consumer (denso, denso_integration_tests) inherits the include
# roots, link graph, and APP_VERSION identically to the old `denso` target.
target_include_directories(denso_app PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} ${OpenCV_INCLUDE_DIRS})
target_link_libraries(denso_app PUBLIC denso_core denso_detection denso_brazing denso_camera Qt6::Widgets Qt6::Multimedia Qt6::Network ${OpenCV_LIBS})
target_compile_definitions(denso_app PUBLIC APP_VERSION="0.1.0")

# The shipped executable: only the entry point + resources live here; everything
# else comes from the denso_app OBJECT library above.
add_executable(denso WIN32
    main.cpp
    resources.qrc
)
target_link_libraries(denso PRIVATE denso_app)

# ── Inference backend, platform-split (see top-level CMakeLists.txt) ──────────
# The backend sources compile into denso_app (so denso_integration_tests links
# them too); the runtime-DLL / model POST_BUILD staging stays on the `denso` exe.
if(WIN32)
    # Windows: ONNX Runtime engine + its runtime DLLs beside the exe.
    target_sources(denso_app PRIVATE detection/ort_engine.cpp)
    target_link_libraries(denso_app PUBLIC onnxruntime)

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
    # The engine source compiles into denso_app; the TRT link + include propagate
    # PUBLICly so both denso and denso_integration_tests inherit them.
    target_sources(denso_app PRIVATE
        detection/trt_engine.cpp)
    target_link_libraries(denso_app PUBLIC ${TRT_NVINFER} CUDA::cudart)
    target_include_directories(denso_app PUBLIC ${TRT_INCLUDE_DIR})
endif()
