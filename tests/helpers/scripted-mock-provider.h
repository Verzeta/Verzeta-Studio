// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#pragma once

#include "api/llm-interface.h"

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

class ScriptedMockProvider : public ILLMProvider {
    Q_OBJECT
  public:
    struct ScriptStep {
        QStringList chunks;
        QString finishReason;
        int tokens = 0;
        bool isError = false;
        QString errorMessage;
        QList<QJsonObject> toolCallsJson;

        QStringList thinkingChunks;
    };

    explicit ScriptedMockProvider(QObject* parent = nullptr);

    ScriptedMockProvider(QString providerId, QStringList models, QObject* parent = nullptr);

    ~ScriptedMockProvider() override;

    QString providerId() const override;
    QString displayName() const override;
    QStringList availableModels() override;
    int contextWindowFor(const QString&) override { return 0; }
    bool supportsStreaming() const override;
    bool supportsToolCalling() const override;
    bool supportsVision() const override;
    void refreshModels() override;
    void cancelRequest() override;
    void sendRequest(const LlmRequest& req) override;

    void setScript(QList<ScriptStep> script);

    void setHoldInFlight(bool held);

    bool hasHeldRequest() const;

    void releaseHeld();

    int stepsConsumed() const;

    const QList<LlmRequest>& capturedRequests() const;

    void emitLateChunk(const LlmChunk& chunk);

    void emitLateRequestFinished(const QString& finishReason, int totalTokens);

  private:
    void emitTerminal(const ScriptStep& step);

    QString m_providerId;
    QStringList m_models;
    QList<ScriptStep> m_script;
    int m_scriptIdx = 0;
    QList<LlmRequest> m_captured;

    bool m_holdInFlight = false;
    bool m_hasHeld = false;
    ScriptStep m_heldStep;
};
