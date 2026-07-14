// Minimal TensorRT logger: forward errors/warnings to the app log, drop the
// (very chatty) info/verbose stream. One instance lives inside each TrtEngine.
#pragma once

#include <NvInferRuntime.h>

#include <QDebug>

namespace denso::ui {

class TrtLogger final : public nvinfer1::ILogger {
public:
    void log(Severity severity, const char* message) noexcept override {
        if (severity == Severity::kINTERNAL_ERROR ||
            severity == Severity::kERROR ||
            severity == Severity::kWARNING) {
            qWarning().noquote() << "[trt]" << (message ? message : "(no message)");
        }
    }
};

} // namespace denso::ui
