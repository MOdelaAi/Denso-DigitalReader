#include "ui/camera/shared/detection/trt_engine.h"

#include "ui/camera/shared/detection/class_names_sidecar.h"
#include "ui/camera/shared/detection/letterbox.h"
#include "ui/camera/shared/detection/yolo_decode.h"

#include <opencv2/dnn.hpp>
#include <opencv2/imgproc.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace denso::ui {
namespace {

std::runtime_error engine_error(const std::string& path, const std::string& reason) {
    return std::runtime_error("TensorRT engine '" + path + "': " + reason);
}

void check_cuda(cudaError_t result, const std::string& path, const char* operation) {
    if (result != cudaSuccess) {
        throw engine_error(path, std::string(operation) + " failed: " +
                                     cudaGetErrorString(result));
    }
}

bool is_input_shape_640(const nvinfer1::Dims& dims) {
    return dims.nbDims == 4 && (dims.d[0] == -1 || dims.d[0] >= 1) &&
           dims.d[1] == 3 && dims.d[2] == 640 && dims.d[3] == 640;
}

bool profile_admits_batch_one(const nvinfer1::Dims& minimum,
                              const nvinfer1::Dims& maximum) {
    return minimum.nbDims == 4 && maximum.nbDims == 4 &&
           minimum.d[0] <= 1 && maximum.d[0] >= 1 &&
           minimum.d[1] <= 3 && maximum.d[1] >= 3 &&
           minimum.d[2] <= 640 && maximum.d[2] >= 640 &&
           minimum.d[3] <= 640 && maximum.d[3] >= 640;
}

std::size_t tensor_elements(const nvinfer1::Dims& dims, const std::string& path,
                            const char* tensor_kind) {
    if (dims.nbDims <= 0) {
        throw engine_error(path, std::string(tensor_kind) + " tensor has invalid rank");
    }
    std::size_t count = 1;
    for (int i = 0; i < dims.nbDims; ++i) {
        if (dims.d[i] <= 0) {
            throw engine_error(path, std::string(tensor_kind) +
                                         " tensor has unresolved or invalid dimensions");
        }
        const auto extent = static_cast<std::size_t>(dims.d[i]);
        if (count > std::numeric_limits<std::size_t>::max() / extent) {
            throw engine_error(path, std::string(tensor_kind) +
                                         " tensor element count overflows size_t");
        }
        count *= extent;
    }
    return count;
}

std::vector<char> read_engine_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) {
        throw engine_error(path, "file is missing or cannot be opened");
    }
    const std::streamsize size = file.tellg();
    if (size <= 0) {
        throw engine_error(path, "file is empty");
    }
    file.seekg(0, std::ios::beg);
    std::vector<char> bytes(static_cast<std::size_t>(size));
    if (!file.read(bytes.data(), size)) {
        throw engine_error(path, "failed to read the complete file");
    }
    return bytes;
}

} // namespace

TrtEngine::TrtEngine(const std::string& engine_path, const std::string& cache_dir)
    : engine_path_(engine_path) {
    (void)cache_dir;

    const auto sidecar = read_names_sidecar(std::filesystem::path(engine_path_));
    if (!sidecar) {
        std::filesystem::path expected(engine_path_);
        expected.replace_extension(".names.json");
        throw engine_error(engine_path_, "class-names sidecar is missing: expected '" +
                                             expected.string() + "'");
    }
    names_ = *sidecar;

    const std::vector<char> serialized = read_engine_file(engine_path_);

    runtime_.reset(nvinfer1::createInferRuntime(logger_));
    if (!runtime_) {
        throw engine_error(engine_path_, "createInferRuntime returned null");
    }

    engine_.reset(runtime_->deserializeCudaEngine(serialized.data(), serialized.size()));
    if (!engine_) {
        throw engine_error(engine_path_,
                           "deserialization failed; rebuild on-device for TRT 10.3 / sm_87");
    }

    int input_count = 0;
    int output_count = 0;
    for (int i = 0; i < engine_->getNbIOTensors(); ++i) {
        const char* name = engine_->getIOTensorName(i);
        if (!name) {
            throw engine_error(engine_path_, "an I/O tensor has no name");
        }
        switch (engine_->getTensorIOMode(name)) {
        case nvinfer1::TensorIOMode::kINPUT:
            ++input_count;
            input_name_ = name;
            break;
        case nvinfer1::TensorIOMode::kOUTPUT:
            ++output_count;
            output_name_ = name;
            break;
        default:
            throw engine_error(engine_path_, "an I/O tensor has an invalid I/O mode");
        }
    }

    if (input_count != 1 || output_count != 1) {
        throw engine_error(engine_path_,
                           "expected exactly 1 input and 1 output tensor, found " +
                               std::to_string(input_count) + " input(s) and " +
                               std::to_string(output_count) + " output(s)");
    }

    if (engine_->getTensorDataType(input_name_.c_str()) != nvinfer1::DataType::kFLOAT ||
        engine_->getTensorDataType(output_name_.c_str()) != nvinfer1::DataType::kFLOAT) {
        throw engine_error(engine_path_, "input and output tensors must both use FP32");
    }

    const nvinfer1::Dims engine_input_shape =
        engine_->getTensorShape(input_name_.c_str());
    if (!is_input_shape_640(engine_input_shape)) {
        throw engine_error(engine_path_, "input tensor must have shape [*,3,640,640]");
    }

    if (engine_input_shape.d[0] == -1) {
        if (engine_->getNbOptimizationProfiles() < 1) {
            throw engine_error(engine_path_, "dynamic input has no optimization profile");
        }
        const nvinfer1::Dims minimum = engine_->getProfileShape(
            input_name_.c_str(), 0, nvinfer1::OptProfileSelector::kMIN);
        const nvinfer1::Dims maximum = engine_->getProfileShape(
            input_name_.c_str(), 0, nvinfer1::OptProfileSelector::kMAX);
        if (!profile_admits_batch_one(minimum, maximum)) {
            throw engine_error(engine_path_,
                               "optimization profile 0 does not admit input [1,3,640,640]");
        }
    } else if (engine_input_shape.d[0] != 1) {
        throw engine_error(engine_path_,
                           "static input batch is not 1 and therefore does not admit batch=1");
    }

    context_.reset(engine_->createExecutionContext());
    if (!context_) {
        throw engine_error(engine_path_, "createExecutionContext returned null");
    }

    const nvinfer1::Dims batch_one{4, {1, 3, kSize, kSize}};
    if (!context_->setInputShape(input_name_.c_str(), batch_one)) {
        throw engine_error(engine_path_, "setInputShape rejected [1,3,640,640]");
    }

    output_shape_ = context_->getTensorShape(output_name_.c_str());
    if (output_shape_.nbDims != 3 || output_shape_.d[0] != 1 ||
        output_shape_.d[1] <= 0 || output_shape_.d[2] <= 0) {
        throw engine_error(engine_path_,
                           "output tensor must resolve to [1,N,6] or "
                           "[1,4+classes,anchors] for batch=1");
    }

    const std::size_t input_elements = static_cast<std::size_t>(3) * kSize * kSize;
    const std::size_t output_elements =
        tensor_elements(output_shape_, engine_path_, "output");

    host_input_.resize(input_elements);
    host_output_.resize(output_elements);

    try {
        check_cuda(cudaStreamCreate(&stream_), engine_path_, "cudaStreamCreate");
        check_cuda(cudaMalloc(&device_input_, host_input_.size() * sizeof(float)),
                   engine_path_, "cudaMalloc(input)");
        check_cuda(cudaMalloc(&device_output_, host_output_.size() * sizeof(float)),
                   engine_path_, "cudaMalloc(output)");
        if (!context_->setTensorAddress(input_name_.c_str(), device_input_)) {
            throw engine_error(engine_path_,
                               "setTensorAddress failed for input '" + input_name_ + "'");
        }
        if (!context_->setTensorAddress(output_name_.c_str(), device_output_)) {
            throw engine_error(engine_path_,
                               "setTensorAddress failed for output '" + output_name_ + "'");
        }
    } catch (...) {
        release_cuda();
        throw;
    }
}

TrtEngine::~TrtEngine() { release_cuda(); }

void TrtEngine::release_cuda() noexcept {
    if (device_output_) {
        cudaFree(device_output_);
        device_output_ = nullptr;
    }
    if (device_input_) {
        cudaFree(device_input_);
        device_input_ = nullptr;
    }
    if (stream_) {
        cudaStreamDestroy(stream_);
        stream_ = nullptr;
    }
}

std::vector<Detection> TrtEngine::infer(const cv::Mat& bgr) {
    if (bgr.empty()) {
        return {};
    }

    std::lock_guard<std::mutex> lock(infer_mutex_);

    // Letterbox → RGB → NCHW float32 [0,1] blob (identical to OrtEngine).
    cv::Mat letterboxed;
    const LetterboxInfo lb = letterbox(bgr, letterboxed, kSize);
    cv::Mat rgb;
    cv::cvtColor(letterboxed, rgb, cv::COLOR_BGR2RGB);
    cv::Mat blob;
    cv::dnn::blobFromImage(rgb, blob, 1.0 / 255.0, cv::Size(kSize, kSize),
                           cv::Scalar(), /*swapRB=*/false, /*crop=*/false, CV_32F);

    if (!blob.isContinuous() || blob.total() != host_input_.size()) {
        throw engine_error(engine_path_, "preprocessing produced an unexpected input buffer");
    }

    const float* blob_data = reinterpret_cast<const float*>(blob.data);
    std::copy(blob_data, blob_data + host_input_.size(), host_input_.begin());

    check_cuda(cudaMemcpyAsync(device_input_, host_input_.data(),
                               host_input_.size() * sizeof(float),
                               cudaMemcpyHostToDevice, stream_),
               engine_path_, "cudaMemcpyAsync(H2D)");

    if (!context_->enqueueV3(stream_)) {
        throw engine_error(engine_path_, "enqueueV3 failed");
    }

    check_cuda(cudaMemcpyAsync(host_output_.data(), device_output_,
                               host_output_.size() * sizeof(float),
                               cudaMemcpyDeviceToHost, stream_),
               engine_path_, "cudaMemcpyAsync(D2H)");
    check_cuda(cudaStreamSynchronize(stream_), engine_path_, "cudaStreamSynchronize");

    const float* output = host_output_.data();

    // End-to-end models emit [1,num_dets,6]; raw YOLO heads emit
    // [1,4+num_classes,num_anchors]. Same branch as OrtEngine.
    if (output_shape_.d[2] == 6) {
        return decode_yolo_end2end(output, static_cast<int>(output_shape_.d[1]), lb,
                                   bgr.cols, bgr.rows, kConfFloor);
    }
    const int num_classes = static_cast<int>(output_shape_.d[1]) - 4;
    const int num_anchors = static_cast<int>(output_shape_.d[2]);
    if (num_classes <= 0 || num_anchors <= 0) {
        throw engine_error(engine_path_,
                           "raw YOLO output has invalid class or anchor dimensions");
    }
    return decode_yolo(output, num_classes, num_anchors, lb, bgr.cols, bgr.rows,
                       kConfFloor, kNmsIou);
}

} // namespace denso::ui
