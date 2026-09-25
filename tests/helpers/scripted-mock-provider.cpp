// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "scripted-mock-provider.h"

#include <utility>

ScriptedMockProvider::ScriptedMockProvider(QObject* parent)
    : ILLMProvider(parent)
    , m_providerId{QStringLiteral("mock")}
    , m_models{QStringLiteral("mock-model")} {}

ScriptedMockProvider::ScriptedMockProvider(QString providerId, QStringList models, QObject* parent)
    : ILLMProvider(parent), m_providerId{std::move(providerId)}, m_models{std::move(models)} {}

ScriptedMockProvider::~ScriptedMockProvider() = default;

QString ScriptedMockProvider::providerId() const {
    return m_providerId;
}

QString ScriptedMockProvider::displayName() const {
    return m_providerId;
}

QStringList ScriptedMockProvider::availableModels() {
    return m_models;
}

bool ScriptedMockProvider::supportsStreaming() const {
    return true;
}
bool ScriptedMockProvider::supportsToolCalling() const {
    return true;
}
bool ScriptedMockProvider::supportsVision() const {
    return false;
}

void ScriptedMockProvider::refreshModels() {
    emit modelsRefreshed(m_models);
}

void ScriptedMockProvider::cancelRequest() {}

void ScriptedMockProvider::setScript(QList<ScriptStep> script) {
    m_script = std::move(script);
    m_scriptIdx = 0;
}

int ScriptedMockProvider::stepsConsumed() const {
    return m_scriptIdx;
}

const QList<LlmRequest>& ScriptedMockProvider::capturedRequests() const {
    return m_captured;
}

void ScriptedMockProvider::emitLateChunk(const LlmChunk& chunk) {
    emit chunkReceived(chunk);
}

void ScriptedMockProvider::emitLateRequestFinished(const QString& finishReason, int totalTokens) {
    emit requestFinished(finishReason, totalTokens);
}

void ScriptedMockProvider::setHoldInFlight(bool held) {
    m_holdInFlight = held;
}

bool ScriptedMockProvider::hasHeldRequest() const {
    return m_hasHeld;
}

void ScriptedMockProvider::releaseHeld() {
    if (!m_hasHeld)
        return;
    m_hasHeld = false;
    const ScriptStep step = m_heldStep;
    emitTerminal(step);
}

void ScriptedMockProvider::emitTerminal(const ScriptStep& step) {
    if (step.isError) {
        emit requestError(step.errorMessage);
    } else {
        emit requestFinished(step.finishReason, step.tokens);
    }
}

void ScriptedMockProvider::sendRequest(const LlmRequest& req) {
    m_captured.append(req);

    if (m_scriptIdx >= m_script.size()) {
        emit requestError(QStringLiteral("Script exhausted"));
        return;
    }
    const ScriptStep step = m_script.at(m_scriptIdx++);

    for (const QString& t : step.thinkingChunks) {
        LlmChunk tc;
        tc.thinkingDelta = t;
        emit chunkReceived(tc);
    }
    for (const QString& delta : step.chunks) {
        LlmChunk c;
        c.delta = delta;
        emit chunkReceived(c);
    }
    if (step.finishReason == QStringLiteral("tool_calls")) {
        for (const QJsonObject& call : step.toolCallsJson) {
            LlmChunk tc;
            tc.toolCallJson = call;
            emit chunkReceived(tc);
        }
    }

    if (m_holdInFlight) {
        m_hasHeld = true;
        m_heldStep = step;
        return;
    }

    emitTerminal(step);
}
