// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file ragp-slot.h
 * @brief The verzeta-inference RAGP model slot: loads one instruct
 *        GGUF via llama.cpp (GPU-first, CPU fallback) and serves one-shot text
 *        completions. It is the sidecar side of RAGP tier-3 classification
 *        and the deferred-action intent confirmer. The generation loop
 *        mirrors the app's proven in-process local RAGP path (greedy
 *        sampling, chunked prompt decode, KV clear per call) so both
 *        engines produce the same completions for the same model file.
 * @layer Tool (separate-process sidecar)
 * @dependencies Qt6::Core; llama.cpp when built with it
 *               (VERZETA_INFER_HAS_LLAMA), otherwise every method
 *               degrades to a structured "built without llama.cpp"
 *               failure.
 */

#pragma once

#include <functional>
#include <QString>

struct llama_model;
struct llama_context;

namespace Verzeta::Infer {

/**
 * @brief One resident instruct model serving synchronous one-shot
 *        completions. Single-threaded by design (per-slot FIFO is the
 *        sidecar's process contract), so there is no internal locking.
 */
class RagpSlot {
  public:
    RagpSlot() = default;
    ~RagpSlot();

    RagpSlot(const RagpSlot&) = delete;
    RagpSlot& operator=(const RagpSlot&) = delete;

    /**
     * @brief Loads (or replaces) the instruct GGUF at @p path with the
     *        same context shape the in-process RAGP engine uses
     *        (n_ctx 4096, n_batch 1024; GPU-first load, CPU fallback).
     * @param path  Absolute path to the .gguf file.
     * @param error Failure reason (set only on false).
     * @param nCtx  Context window for the created llama context
     *              (clamped to [512, 131072]; the chat slot passes the
     *              request's window, RAGP keeps the 4096 default).
     * @returns True when the model + context are ready.
     */
    bool load(const QString& path, QString* error, int nCtx = 4096);

    /** @brief Releases the model + context; safe when not loaded. */
    void unload();

    /**
     * @brief Reports whether a model is currently resident.
     * @returns True while loaded.
     */
    bool isLoaded() const;

    /**
     * @brief Identifier of the loaded model.
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
     * @brief Runs one greedy completion of @p prompt.
     *        The KV cache is cleared first, the prompt is decoded in
     *        n_batch chunks, and generation stops on end-of-generation,
     *        @p maxTokens, or (when @p jsonStop is set) the first
     *        brace-balanced `}` after an opening `{` (the classify
     *        early-stop; saves tokens on strict-JSON prompts).
     * @param prompt       Full prompt text (the HOST builds prompt
     *                     BODIES; the sidecar generates and, when
     *                     asked, wraps the body in the model's chat
     *                     template, because only the sidecar holds the
     *                     model handle).
     * @param maxTokens    Generation ceiling (clamped to [1, 1024]).
     * @param jsonStop     Enable the brace-balance early stop.
     * @param chatTemplate Wrap @p prompt as a single user message in
     *                     the loaded model's own chat template (ChatML
     *                     fallback when the GGUF ships none). Required
     *                     for instruct models given a bare prompt body;
     *                     without it small models derail (a live
     *                     LFM2.5 emitted a lone markdown fence + EOG
     *                     on 76 % of classify calls).
     * @param outText      Receives the generated text (valid on true).
     * @param error        Failure reason (set only on false).
     * @returns True when generation completed (possibly empty text on
     *          an immediate end-of-generation token); false on
     *          not-loaded / tokenize / decode failures.
     */
    bool complete(const QString& prompt,
                  int maxTokens,
                  bool jsonStop,
                  bool chatTemplate,
                  QString* outText,
                  QString* error);

    /**
     * @brief Streaming completion for the chat workload: temperature
     *        sampling (greedy when @p temperature <= 0), one
     *        @p onPiece callback per decoded token piece, cooperative
     *        cancellation via @p shouldCancel checked between tokens
     *        (how the sidecar honours a mid-stream cancel frame).
     * @param prompt       Full prompt text (host-side formatting).
     * @param maxTokens    Generation ceiling (clamped to [1, 32768]).
     * @param temperature  Sampling temperature; <= 0 ⇒ greedy.
     * @param onPiece      Invoked with each generated text piece.
     * @param shouldCancel Polled between tokens; true stops generation.
     * @param outFinish    "stop" | "length" | "cancelled" on success.
     * @param error        Failure reason (set only on false).
     * @returns True when generation ended normally (incl. cancelled);
     *          false on not-loaded / tokenize / decode failures.
     */
    bool completeStream(const QString& prompt,
                        int maxTokens,
                        double temperature,
                        const std::function<void(const QString&)>& onPiece,
                        const std::function<bool()>& shouldCancel,
                        QString* outFinish,
                        QString* error);

  private:
    llama_model* m_model = nullptr;
    llama_context* m_ctx = nullptr;
    QString m_loadedPath;
    QString m_accel;
};

}  // namespace Verzeta::Infer
