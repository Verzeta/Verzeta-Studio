// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file llamacpp-provider.cpp
 * @brief Implementation of the sidecar-bridge local chat
 *        provider. The former in-process engine (backend init, model
 *        load, worker thread, decode loop) was MOVED into the
 *        verzeta-inference sidecar; this file keeps orchestration
 *        only: model selection, directory scan, ChatML prompt
 *        formatting, stream relay through the inline-\<think\> hoister,
 *        and cancel forwarding.
 * @layer API
 * @dependencies InferenceSidecarHost (stream API), Qt6::Core.
 */

#include "llamacpp-provider.h"

#include "../services/inference-sidecar-host.h"
#include "../utils/logger.h"

#include <QDir>
#include <QFileInfo>

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

LlamaCppProvider::LlamaCppProvider(QObject* parent) : ILLMProvider(parent) {}

LlamaCppProvider::~LlamaCppProvider() {
    if (m_activeStreamId != 0 && m_host) {
        m_host->cancelChatStream(m_activeStreamId);
    }
}

// ---------------------------------------------------------------------------
// Host wiring
// ---------------------------------------------------------------------------

void LlamaCppProvider::setSidecarHost(Verzeta::Infer::InferenceSidecarHost* host) {
    m_host = host;
    m_hostConnected = false;
    ensureHostConnections();
}

void LlamaCppProvider::ensureHostConnections() {
    if (m_hostConnected || !m_host)
        return;
    m_hostConnected = true;

    connect(m_host,
            &Verzeta::Infer::InferenceSidecarHost::chatStreamChunk,
            this,
            [this](quint64 streamId, const QString& delta) {
                if (streamId != m_activeStreamId || m_activeStreamId == 0)
                    return;
                // Route through the inline-<think> hoister so reasoning GGUFs
                // (Qwen 3 quants, DeepSeek-R1 quants, gpt-oss-thinking) emit
                // thinking into the dedicated sidecar channel instead of
                // leaking into content. Pass-through for everything else.
                const auto hoist = m_thinkingHoister.feed(delta);
                if (hoist.content.isEmpty() && hoist.thinking.isEmpty())
                    return;
                LlmChunk chunk;
                chunk.delta = hoist.content;
                chunk.thinkingDelta = hoist.thinking;
                emit chunkReceived(chunk);
            });

    connect(m_host,
            &Verzeta::Infer::InferenceSidecarHost::chatStreamFinished,
            this,
            [this](quint64 streamId,
                   bool ok,
                   const QString& finishReason,
                   int tokens,
                   const QString& error) {
                if (streamId != m_activeStreamId || m_activeStreamId == 0)
                    return;
                m_activeStreamId = 0;
                if (!ok) {
                    emit requestError(
                        error.isEmpty()
                            ? QStringLiteral("Local inference failed while generating the reply.")
                            : error);
                    return;
                }
                LlmChunk finalChunk;
                finalChunk.finishReason = finishReason;
                finalChunk.tokenCountEstimate = tokens;
                emit chunkReceived(finalChunk);
                emit requestFinished(finishReason, tokens);
            });
}

// ---------------------------------------------------------------------------
// Model selection
// ---------------------------------------------------------------------------

void LlamaCppProvider::loadModel(const QString& modelPath) {
    // Selection only — the sidecar loads lazily on the first request
    // and keeps the model resident (same UX, no app RAM).
    m_modelPath = modelPath;
    qCInfo(verzetaLlm) << "LlamaCppProvider: model selected" << QFileInfo(modelPath).fileName()
                       << "(served by verzeta-inference)";
}

void LlamaCppProvider::unloadModel() {
    m_modelPath.clear();
}

bool LlamaCppProvider::isModelLoaded() const {
    return !m_modelPath.isEmpty() && m_host && m_host->isAvailable();
}

void LlamaCppProvider::setModelDir(const QString& dirPath) {
    m_modelDir = dirPath;
    refreshModels();
}

// ---------------------------------------------------------------------------
// ILLMProvider surface
// ---------------------------------------------------------------------------

QString LlamaCppProvider::providerId() const {
    return QStringLiteral("llamacpp");
}

QString LlamaCppProvider::displayName() const {
    return QStringLiteral("llama.cpp (local)");
}

QStringList LlamaCppProvider::availableModels() {
    return m_availableModels;
}

void LlamaCppProvider::refreshModels() {
    m_availableModels.clear();
    if (m_modelDir.isEmpty()) {
        emit modelsRefreshed(m_availableModels);
        return;
    }
    QDir dir(m_modelDir);
    const QStringList ggufs =
        dir.entryList(QStringList{QStringLiteral("*.gguf")}, QDir::Files, QDir::Name);
    m_availableModels = ggufs;
    emit modelsRefreshed(m_availableModels);
}

bool LlamaCppProvider::supportsStreaming() const {
    return true;
}
bool LlamaCppProvider::supportsToolCalling() const {
    return false;
}
bool LlamaCppProvider::supportsVision() const {
    return false;
}

void LlamaCppProvider::sendRequest(const LlmRequest& req) {
    if (m_modelPath.isEmpty()) {
        emit requestError(QStringLiteral("No local model is loaded. Choose a model file in the "
                                         "llama.cpp provider settings first."));
        return;
    }
    if (!m_host || !m_host->isAvailable()) {
        emit requestError(
            QStringLiteral("Local inference is unavailable because the verzeta-inference "
                           "engine is not running."));
        return;
    }
    ensureHostConnections();

    if (m_activeStreamId != 0) {
        m_host->cancelChatStream(m_activeStreamId);
        m_activeStreamId = 0;
    }
    m_thinkingHoister.reset();

    const QString prompt = buildPrompt(req);
    m_lastRequestedCtx =
        req.config.contextWindow > 0 ? req.config.contextWindow : m_lastRequestedCtx;
    const int maxTokens = req.config.maxTokens > 0 ? req.config.maxTokens : 1024;
    m_activeStreamId = m_host->startChatStream(
        prompt, m_modelPath, m_lastRequestedCtx, maxTokens, req.config.temperature);
    if (m_activeStreamId == 0) {
        emit requestError(
            QStringLiteral("Local inference is shutting down, so the request was not started."));
    }
}

void LlamaCppProvider::cancelRequest() {
    if (m_activeStreamId != 0 && m_host) {
        m_host->cancelChatStream(m_activeStreamId);
    }
}

int LlamaCppProvider::contextWindowFor(const QString& model) {
    Q_UNUSED(model);
    return m_lastRequestedCtx;
}

// ---------------------------------------------------------------------------
// Prompt construction (unchanged — app-side, engine-free)
// ---------------------------------------------------------------------------

/**
 * @brief Builds a prompt string from the request in ChatML format.
 * @param req The LLM request.
 * @return Prompt string in a simple user/assistant chat format.
 *
 * Uses a generic ChatML-compatible format. For production use with specific
 * model families (Llama3, Mistral, etc.), the format should match the
 * model's expected chat template.
 */
QString LlamaCppProvider::buildPrompt(const LlmRequest& req) {
    QString prompt;

    // System prompt
    if (!req.systemPrompt.isEmpty()) {
        prompt += QStringLiteral("<|im_start|>system\n");
        prompt += req.systemPrompt;
        prompt += QStringLiteral("\n<|im_end|>\n");
    }

    // Conversation messages
    for (const LlmMessage& msg : req.messages) {
        prompt += QStringLiteral("<|im_start|>");
        prompt += msg.role;
        prompt += QStringLiteral("\n");
        prompt += msg.content;
        prompt += QStringLiteral("\n<|im_end|>\n");
    }

    // Start the assistant turn
    prompt += QStringLiteral("<|im_start|>assistant\n");

    return prompt;
}
