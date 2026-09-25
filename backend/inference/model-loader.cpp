// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file model-loader.cpp
 * @brief Implementation of the shared GPU-first model loader. The
 *        two-attempt sequence (all layers offloaded → CPU-only retry)
 *        is a verbatim port of the load policy the app's in-process
 *        engine used before the sidecar move; keeping it here restores
 *        engine parity for every slot with one code path.
 * @layer Tool (separate-process sidecar)
 * @dependencies llama.cpp (guarded); Qt6::Core.
 */

#include "model-loader.h"

#ifdef VERZETA_INFER_HAS_LLAMA

#include <algorithm>
#include <cstdio>
#include <ggml-backend.h>
#include <llama.h>
#include <thread>

namespace Verzeta::Infer {

namespace {

/**
 * @brief One load attempt with the given offload count.
 * @param path       Model file path (UTF-8 converted here).
 * @param nGpuLayers Layer count to offload (99 = everything, 0 = CPU).
 * @returns The model, or nullptr on failure.
 */
llama_model* tryLoad(const QString& path, int nGpuLayers) {
    llama_model_params mparams = llama_model_default_params();
    mparams.n_gpu_layers = nGpuLayers;
    return llama_model_load_from_file(path.toUtf8().constData(), mparams);
}

/**
 * @brief Name of the first GPU-class ggml device, or empty when the
 *        runtime has none. Offload with n_gpu_layers=99 only happens
 *        when such a device exists, so this is the honest source for the
 *        acceleration label (a successful attempt 1 on a GPU-less box
 *        ran fully on CPU).
 * @returns Device name (e.g. "Vulkan0"), or empty.
 */
QString firstGpuDeviceName() {
    const size_t n = ggml_backend_dev_count();
    for (size_t i = 0; i < n; ++i) {
        ggml_backend_dev_t dev = ggml_backend_dev_get(i);
        const enum ggml_backend_dev_type t = ggml_backend_dev_type(dev);
        if (t == GGML_BACKEND_DEVICE_TYPE_GPU || t == GGML_BACKEND_DEVICE_TYPE_IGPU) {
            return QString::fromUtf8(ggml_backend_dev_name(dev));
        }
    }
    return {};
}

}  // namespace

llama_model* loadModelGpuFirst(const QString& path, QString* accelOut) {
    if (accelOut)
        *accelOut = QStringLiteral("cpu");

    std::fprintf(stderr,
                 "verzeta-inference: loading %s (attempt 1/2: GPU "
                 "offload n_gpu_layers=99)\n",
                 path.toUtf8().constData());
    if (llama_model* m = tryLoad(path, 99)) {
        const QString gpuDev = firstGpuDeviceName();
        if (accelOut && !gpuDev.isEmpty()) {
            *accelOut = QStringLiteral("gpu (%1)").arg(gpuDev);
        }
        std::fprintf(stderr,
                     "verzeta-inference: model loaded (%s)\n",
                     gpuDev.isEmpty() ? "CPU — no GPU device present" : qPrintable(gpuDev));
        return m;
    }

    std::fprintf(stderr,
                 "verzeta-inference: GPU load failed — retrying "
                 "CPU-only (attempt 2/2: n_gpu_layers=0)\n");
    if (llama_model* m = tryLoad(path, 0)) {
        std::fprintf(stderr, "verzeta-inference: model loaded (CPU fallback)\n");
        return m;
    }

    std::fprintf(stderr, "verzeta-inference: both GPU and CPU loads failed\n");
    return nullptr;
}

unsigned int cpuThreadCount() {
    return std::max(1u, std::thread::hardware_concurrency());
}

}  // namespace Verzeta::Infer

#endif  // VERZETA_INFER_HAS_LLAMA
