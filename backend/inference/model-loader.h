// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file model-loader.h
 * @brief Shared GGUF model loading for the verzeta-inference slots:
 *        GPU-first with CPU fallback, mirroring the engine the app
 *        used before the sidecar move (attempt 1 offloads all layers
 *        to the best available GPU backend, attempt 2 retries fully
 *        on CPU). One implementation serves the embed, ragp and chat
 *        slots so the load policy can never drift between them.
 * @layer Tool (separate-process sidecar)
 * @dependencies llama.cpp (guarded by VERZETA_INFER_HAS_LLAMA);
 *               Qt6::Core.
 */

#pragma once

#include <QString>

struct llama_model;

namespace Verzeta::Infer {

#ifdef VERZETA_INFER_HAS_LLAMA

/**
 * @brief Loads the GGUF at @p path GPU-first (n_gpu_layers=99), then
 *        retries CPU-only (n_gpu_layers=0) when the GPU attempt fails.
 *        Both attempts are reported on stderr (relayed to the host's
 *        verzeta.infer channel). A GPU-side abort can only take down
 *        the sidecar process; the host's failure latch and the
 *        rule/remote fallbacks contain it.
 * @param path Absolute path to the .gguf file (existence checked by
 *             the caller).
 * @param accelOut Optional: receives the acceleration that actually
 *                 applies to this load: "gpu (<device>)" when the GPU
 *                 attempt succeeded AND a GPU-class ggml device exists
 *                 (n_gpu_layers is a request, not a guarantee: with no
 *                 GPU device present attempt 1 still succeeds fully on
 *                 CPU, so the label is derived from the device table,
 *                 never from which attempt won), otherwise "cpu".
 * @returns The loaded model, or nullptr when BOTH attempts failed.
 */
llama_model* loadModelGpuFirst(const QString& path, QString* accelOut = nullptr);

/**
 * @brief Thread count for CPU-side decode work: every hardware thread,
 *        matching the pre-sidecar in-process engine. The GPU path
 *        ignores this value.
 * @returns At least 1.
 */
unsigned int cpuThreadCount();

#endif  // VERZETA_INFER_HAS_LLAMA

}  // namespace Verzeta::Infer
