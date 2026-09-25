// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file embed-slot.h
 * @brief The verzeta-inference EMBED model slot: loads one embedding
 *        GGUF via llama.cpp (GPU-first, CPU fallback) and produces float32 vectors
 *        for batches of texts. The inference code deliberately mirrors
 *        the app's proven in-process llama embedding path so the two
 *        backends produce identical vectors for the same model file,
 *        pinned by the sidecar parity test.
 * @layer Tool (separate-process sidecar)
 * @dependencies Qt6::Core; llama.cpp when the sidecar is built with it
 *               (VERZETA_INFER_HAS_LLAMA), otherwise every method
 *               degrades to a structured "built without llama.cpp"
 *               failure and the binary stays a valid protocol peer.
 */

#pragma once

#include <QByteArray>
#include <QString>
#include <QStringList>

struct llama_model;
struct llama_context;

namespace Verzeta::Infer {

/**
 * @brief One resident embedding model: load once, embed many, unload
 *        on demand. Single-threaded by design: the sidecar serves one
 *        request at a time per slot (per-slot FIFO is the process
 *        contract), so no internal locking is needed.
 */
class EmbedSlot {
  public:
    EmbedSlot() = default;
    ~EmbedSlot();

    EmbedSlot(const EmbedSlot&) = delete;
    EmbedSlot& operator=(const EmbedSlot&) = delete;

    /**
     * @brief Loads (or replaces) the embedding GGUF at @p path.
     *        A loaded slot is released first, so switching models
     *        never double-holds RAM.
     * @param path  Absolute path to the .gguf file.
     * @param error Failure reason (set only on false).
     * @returns True when the model + context are ready.
     */
    bool load(const QString& path, QString* error);

    /** @brief Releases the model + context; safe when not loaded. */
    void unload();

    /**
     * @brief Reports whether a model is currently resident.
     * @returns True while loaded.
     */
    bool isLoaded() const;

    /**
     * @brief Identifier of the loaded model, as stored with produced
     *        vectors for retrieval filtering.
     * @returns The model file's basename, or empty when not loaded.
     */
    QString modelId() const;

    /**
     * @brief Acceleration that actually applies to the loaded model,
     *        as reported by the shared loader from the ggml device
     *        table ("gpu (<device>)" or "cpu").
     * @returns The acceleration label, or empty when not loaded.
     */
    QString accel() const;

    /**
     * @brief Embedding dimensionality of the loaded model.
     * @returns The dimension, or 0 when not loaded.
     */
    int dim() const;

    /**
     * @brief Embeds every text in order, appending each vector's raw
     *        float32 bytes to @p outBlob (count × dim floats total).
     * @param texts   Input texts (must be non-empty strings).
     * @param outBlob Receives the concatenated little-endian float32
     *                vectors (native order; host and sidecar share
     *                the machine).
     * @param error   Failure reason (set only on false).
     * @returns True when EVERY text embedded; false aborts on the
     *          first failure with @p outBlob left partially filled
     *          (callers discard it on failure).
     */
    bool embed(const QStringList& texts, QByteArray* outBlob, QString* error);

  private:
    llama_model* m_model = nullptr;
    llama_context* m_ctx = nullptr;
    QString m_loadedPath;
    QString m_accel;
};

}  // namespace Verzeta::Infer
