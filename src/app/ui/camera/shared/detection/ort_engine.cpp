#include "ui/camera/shared/detection/ort_engine.h"

#include "ui/camera/shared/detection/letterbox.h"
#include "ui/camera/shared/detection/names_metadata.h"
#include "ui/camera/shared/detection/yolo_decode.h"

#include <QDebug>

#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>

#include <array>

namespace denso::ui {

namespace {

// Build a session for the requested provider tier; returns nullptr on failure.
// tier: 0 = TensorRT+CUDA, 1 = CUDA, 2 = CPU only.
std::unique_ptr<Ort::Session> make_session(Ort::Env& env, const std::wstring& path,
                                           int tier, const std::string& cache_dir) {
    try {
        Ort::SessionOptions opts;
        opts.SetIntraOpNumThreads(1);
        opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        if (tier == 0) {
            OrtTensorRTProviderOptions trt{};
            trt.device_id = 0;
            trt.trt_engine_cache_enable = 1;
            trt.trt_engine_cache_path = cache_dir.c_str();
            opts.AppendExecutionProvider_TensorRT(trt);
            OrtCUDAProviderOptions cuda{};
            cuda.device_id = 0;
            opts.AppendExecutionProvider_CUDA(cuda);
        } else if (tier == 1) {
            OrtCUDAProviderOptions cuda{};
            cuda.device_id = 0;
            opts.AppendExecutionProvider_CUDA(cuda);
        }
        return std::make_unique<Ort::Session>(env, path.c_str(), opts);
    } catch (const Ort::Exception& e) {
        qWarning().noquote() << "[ort] tier" << tier << "failed:" << e.what();
        return nullptr;
    }
}

std::wstring widen(const std::string& s) {
    return std::wstring(s.begin(), s.end());  // ASCII paths only (models/*.onnx)
}

} // namespace

OrtEngine::OrtEngine(const std::string& model_path, const std::string& engine_cache_dir)
    : env_(ORT_LOGGING_LEVEL_WARNING, "denso") {
    const std::wstring wpath = widen(model_path);
    for (int tier = 0; tier <= 2 && !session_; ++tier) {
        session_ = make_session(env_, wpath, tier, engine_cache_dir);
        if (session_) {
            qInfo().noquote() << "[ort] loaded" << QString::fromStdString(model_path)
                              << "tier" << tier;
        }
    }
    if (!session_) {
        return;
    }
    input_name_ = session_->GetInputNameAllocated(0, alloc_).get();
    output_name_ = session_->GetOutputNameAllocated(0, alloc_).get();

    Ort::ModelMetadata md = session_->GetModelMetadata();
    Ort::AllocatedStringPtr names = md.LookupCustomMetadataMapAllocated("names", alloc_);
    if (names) {
        names_ = parse_names_metadata(names.get());
    }
}

std::vector<std::string> OrtEngine::read_names(const std::string& model_path) {
    try {
        Ort::Env env(ORT_LOGGING_LEVEL_WARNING, "denso-meta");
        Ort::SessionOptions opts;
        Ort::Session s(env, widen(model_path).c_str(), opts);  // CPU
        Ort::AllocatorWithDefaultOptions alloc;
        Ort::ModelMetadata md = s.GetModelMetadata();
        Ort::AllocatedStringPtr names =
            md.LookupCustomMetadataMapAllocated("names", alloc);
        return names ? parse_names_metadata(names.get()) : std::vector<std::string>{};
    } catch (const Ort::Exception&) {
        return {};
    }
}

std::vector<Detection> OrtEngine::infer(const cv::Mat& bgr) {
    if (!session_ || bgr.empty()) {
        return {};
    }
    // Letterbox → RGB → NCHW float32 [0,1] blob.
    cv::Mat lb_img;
    const LetterboxInfo lb = letterbox(bgr, lb_img, kSize);
    cv::Mat rgb;
    cv::cvtColor(lb_img, rgb, cv::COLOR_BGR2RGB);
    cv::Mat blob;
    cv::dnn::blobFromImage(rgb, blob, 1.0 / 255.0, cv::Size(kSize, kSize),
                           cv::Scalar(), /*swapRB=*/false, /*crop=*/false, CV_32F);

    const std::array<int64_t, 4> in_shape{1, 3, kSize, kSize};
    Ort::MemoryInfo mem = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
    Ort::Value input = Ort::Value::CreateTensor<float>(
        mem, reinterpret_cast<float*>(blob.data),
        static_cast<size_t>(blob.total()), in_shape.data(), in_shape.size());

    const char* in_names[] = {input_name_.c_str()};
    const char* out_names[] = {output_name_.c_str()};
    std::vector<Ort::Value> outputs;
    try {
        outputs = session_->Run(Ort::RunOptions{nullptr}, in_names, &input, 1,
                                out_names, 1);
    } catch (const Ort::Exception& e) {
        qWarning().noquote() << "[ort] run failed:" << e.what();
        return {};
    }

    // Output shape [1, 4+nc, na].
    const auto info = outputs[0].GetTensorTypeAndShapeInfo();
    const std::vector<int64_t> shape = info.GetShape();  // {1, 4+nc, na}
    const int num_classes = static_cast<int>(shape[1]) - 4;
    const int num_anchors = static_cast<int>(shape[2]);
    const float* out = outputs[0].GetTensorData<float>();

    return decode_yolo(out, num_classes, num_anchors, lb, bgr.cols, bgr.rows,
                       kConfFloor, kNmsIou);
}

} // namespace denso::ui
