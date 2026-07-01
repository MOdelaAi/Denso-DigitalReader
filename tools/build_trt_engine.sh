#!/usr/bin/env bash
# Build a TensorRT .engine from a models/*.onnx, ahead of time.
#
# TensorRT engines are built here rather than at runtime on purpose: the
# TensorRT EP's first-inference build is a minutes-long, non-interruptible hang
# (why it was dropped from the live path). Building offline with trtexec makes
# the .engine loadable directly.
#
# An engine is specific to THIS GPU architecture (RTX 4070 SUPER = sm89) and
# THIS TensorRT version (10.4). The Jetson (and any other GPU/TRT) needs its own
# build — do NOT ship this .engine to a different target.
#
# Usage:   tools/build_trt_engine.sh <model-stem> [fp16|fp32]
# Example: tools/build_trt_engine.sh yolov8n            # FP16 (default)
#          tools/build_trt_engine.sh denso     fp32     # exact precision
set -euo pipefail

STEM="${1:?usage: build_trt_engine.sh <model-stem> [fp16|fp32]}"
PREC="${2:-fp16}"

# Toolchain locations (adjust here if TensorRT/CUDA move).
TRT="D:/src/TensorRT-10.4.0.26"
CUDA="/c/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v12.6/bin"
GPUEP="$(cd "$(dirname "$0")/.." && pwd)/third_party/gpu_ep"   # staged cuDNN/TRT DLLs
MODELS="$(cd "$(dirname "$0")/.." && pwd)/models"

export PATH="$TRT/lib:$TRT/bin:$CUDA:$GPUEP:$PATH"

ONNX="$MODELS/$STEM.onnx"
ENGINE="$MODELS/$STEM.engine"
[ -f "$ONNX" ] || { echo "no such model: $ONNX" >&2; exit 1; }

FLAG=""
[ "$PREC" = "fp16" ] && FLAG="--fp16"
[ "$PREC" = "fp32" ] && FLAG=""

echo ">> building $STEM.onnx -> $STEM.engine ($PREC, sm89)"
"$TRT/bin/trtexec.exe" --onnx="$ONNX" --saveEngine="$ENGINE" $FLAG
echo ">> done: $ENGINE"
ls -la "$ENGINE"
