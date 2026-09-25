// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file llamacpp-provider.h
 * @brief Local llama.cpp chat provider, a PURE BRIDGE to the
 *        verzeta-inference sidecar (the app's single llama.cpp
 *        integration). Keeps the exact ILLMProvider surface, the
 *        ChatML prompt formatting, and the inline-\<think\> hoisting the
 *        provider has always done; token generation streams back from
 *        the sidecar's chat slot. No llama code, no llama linkage, no
 *        model RAM in the app, so the provider exists (and can
 *        work) in EVERY build.
 * @layer API
 * @dependencies ILLMProvider (API), Verzeta::Infer::InferenceSidecarHost
 *               (async stream API), Chat::InlineThinkingHoister, Qt6::Core.
 */

#pragma once

#include "../services/chat/inline-thinking-hoister.h"
#include "llm-interface.h"

#include <QPointer>
#include <QString>
#include <QStringList>

namespace Verzeta::Infer {
class InferenceSidecarHost;
}

/**
 * @brief ILLMProvider for user-supplied local GGUF chat models, served
 *        by the verzeta-inference sidecar.
 *
 * Single-request semantics (unchanged): one stream at a time; a new
 * sendRequest while a stream is live cancels the old stream first.
 * Main-thread only: the host's stream signals arrive queued on the
 * main thread and are relayed through the thinking hoister into the
 * provider's chunk/finished/error signals.
 */
class LlamaCppProvider : public ILLMProvider {
    Q_OBJECT

  public:
    /**
     * @brief Constructs the provider. Does not touch any model.
     * @param parent Optional Qt parent.
     */
    explicit LlamaCppProvider(QObject* parent = nullptr);

    /**
     * @brief Destructor. Cancels a live stream (best effort).
     */
    ~LlamaCppProvider() override;

    /**
     * @brief Attaches the inference-sidecar host serving the chat
     *        slot. Non-owning; AppController-owned and outliving this
     *        provider. Without a host every request fails with a
     *        structured error (same shape as no-model-loaded).
     * @param host Sidecar host; nullptr detaches.
     */
    void setSidecarHost(Verzeta::Infer::InferenceSidecarHost* host);

    /**
     * @brief Selects the GGUF model file this provider serves. The
     *        sidecar loads it lazily on the first request (and keeps
     *        it resident), so this call is instant.
     * @param modelPath Absolute path to the .gguf file.
     */
    void loadModel(const QString& modelPath);

    /**
     * @brief Clears the selected model (the sidecar keeps whatever it
     *        has resident until another model replaces it).
     */
    void unloadModel();

    /**
     * @brief Whether a model file is selected and the sidecar host is
     *        attached (i.e. a request is worth attempting).
     * @return true when requests can be served.
     */
    bool isModelLoaded() const;

    /**
     * @brief Sets the directory to scan for .gguf model files.
     * @param dirPath Absolute path to the directory.
     */
    void setModelDir(const QString& dirPath);

    // -----------------------------------------------------------------------
    // ILLMProvider interface
    // -----------------------------------------------------------------------

    /**
     * @brief Returns the provider id used by ModelRouter for dispatch.
     * @returns "llamacpp".
     */
    QString providerId() const override;

    /**
     * @brief Human-readable provider name for the UI.
     * @returns Display name string.
     */
    QString displayName() const override;

    /**
     * @brief Model filenames found in the configured directory.
     * @returns Cached list from the last refreshModels().
     */
    QStringList availableModels() override;

    /**
     * @brief Rescans the models directory for .gguf files.
     */
    void refreshModels() override;

    /**
     * @brief Streaming capability flag.
     * @returns true. Deltas stream from the sidecar chunk frames.
     */
    bool supportsStreaming() const override;

    /**
     * @brief Tool-calling capability flag.
     * @returns false (unchanged).
     */
    bool supportsToolCalling() const override;

    /**
     * @brief Vision capability flag.
     * @returns false (unchanged).
     */
    bool supportsVision() const override;

    /**
     * @brief Starts one streaming completion for @p req via the
     *        sidecar chat slot. Emits chunkReceived per delta and
     *        exactly one requestFinished / requestError.
     * @param req Complete LLM request payload.
     */
    void sendRequest(const LlmRequest& req) override;

    /**
     * @brief Cancels the live stream (the sidecar stops within one
     *        token); the stream terminates via requestFinished with
     *        finish reason "cancelled".
     */
    void cancelRequest() override;

    /**
     * @brief Context window for @p model: the window this provider
     *        last requested from the sidecar (the request config's
     *        window, falling back to the provider default).
     * @param model Model name (unused; this is a single-model provider).
     * @returns Context window in tokens.
     */
    int contextWindowFor(const QString& model) override;

  private:
    /**
     * @brief Formats the request into the generic ChatML prompt this
     *        provider has always used (app-side, engine-free, and
     *        unchanged by the sidecar move).
     * @param req The LLM request.
     * @return Prompt string.
     */
    QString buildPrompt(const LlmRequest& req);

    /**
     * @brief Connects the host's stream signals once (idempotent).
     */
    void ensureHostConnections();

    QString m_modelPath;
    QString m_modelDir;
    QStringList m_availableModels;

    QPointer<Verzeta::Infer::InferenceSidecarHost> m_host;
    quint64 m_activeStreamId = 0;  ///< 0 = no live stream
    int m_lastRequestedCtx = 8192;
    bool m_hostConnected = false;

    Chat::InlineThinkingHoister m_thinkingHoister;
};
