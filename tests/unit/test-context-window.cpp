// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/api/anthropic-provider.h"
#include "../../backend/api/deepseek-provider.h"
#include "../../backend/api/gemini-provider.h"
#include "../../backend/api/openai-provider.h"
#include "../../backend/models/llm-config.h"
#include "../../backend/utils/http-client.h"

#include <QtTest>

#include <QString>

class TestContextWindow : public QObject {
    Q_OBJECT

  private slots:

    void test_OpenAI_PublishedMap() {
        HttpClient http;
        OpenAIProvider p(http);
        QCOMPARE(p.contextWindowFor(QStringLiteral("gpt-4o")), 128000);
        QCOMPARE(p.contextWindowFor(QStringLiteral("gpt-4o-mini")), 128000);
        QCOMPARE(p.contextWindowFor(QStringLiteral("gpt-4.1")), 128000);
        QCOMPARE(p.contextWindowFor(QStringLiteral("o3-mini")), 128000);
        QCOMPARE(p.contextWindowFor(QStringLiteral("o4-mini")), 128000);
        QCOMPARE(p.contextWindowFor(QStringLiteral("gpt-4-turbo")), 128000);
        QCOMPARE(p.contextWindowFor(QStringLiteral("gpt-3.5-turbo")), 16385);
        QCOMPARE(p.contextWindowFor(QStringLiteral("gpt-4-32k")), 32768);
        QCOMPARE(p.contextWindowFor(QStringLiteral("gpt-4")), 8192);
        QCOMPARE(p.contextWindowFor(QStringLiteral("some-future-model")), 128000);
        QCOMPARE(p.contextWindowFor(QString()), 0);
    }


    void test_Anthropic_PublishedMap() {
        HttpClient http;
        AnthropicProvider p(http);
        QCOMPARE(p.contextWindowFor(QStringLiteral("claude-opus-4-6")), 200000);
        QCOMPARE(p.contextWindowFor(QStringLiteral("claude-3-5-sonnet-20241022")), 200000);
        QCOMPARE(p.contextWindowFor(QStringLiteral("claude-haiku-4-5")), 200000);
    }


    void test_Gemini_PublishedMap() {
        HttpClient http;
        GeminiProvider p(http);
        QCOMPARE(p.contextWindowFor(QStringLiteral("gemini-1.5-pro")), 1000000);
        QCOMPARE(p.contextWindowFor(QStringLiteral("gemini-1.5-flash")), 1000000);
        QCOMPARE(p.contextWindowFor(QStringLiteral("gemini-2.0-flash")), 1000000);
        QCOMPARE(p.contextWindowFor(QStringLiteral("gemini-2.5-pro")), 1000000);
        QCOMPARE(p.contextWindowFor(QStringLiteral("gemini-exp-1206")), 1000000);
        QCOMPARE(p.contextWindowFor(QStringLiteral("gemini-1.0-pro")), 1000000);
        QCOMPARE(p.contextWindowFor(QStringLiteral("gemini-nano")), 32768);
    }


    void test_DeepSeek_PublishedMap() {
        HttpClient http;
        DeepSeekProvider p(http);
        QCOMPARE(p.contextWindowFor(QStringLiteral("deepseek-chat")), 131072);
        QCOMPARE(p.contextWindowFor(QStringLiteral("deepseek-reasoner")), 131072);
        QCOMPARE(p.contextWindowFor(QStringLiteral("deepseek-anything")), 131072);
    }


    void test_Policy_UserOverrideHonoredVerbatim() {
        QCOMPARE(effectiveContextWindow(131072, 16384, 16384, LlmConfig::kSaneCeilingCtx), 16384);
        QCOMPARE(effectiveContextWindow(0, 99999, 16384, LlmConfig::kSaneCeilingCtxHeavy), 99999);
        QCOMPARE(effectiveContextWindow(131072, 200000, 16384, LlmConfig::kSaneCeilingCtx), 200000);
        QCOMPARE(effectiveContextWindow(0, 4096, 16384, LlmConfig::kSaneCeilingCtx), 4096);
    }

    void test_Policy_ResolvedClampedToRoleAwareCeiling() {
        QCOMPARE(effectiveContextWindow(
                     131072, LlmConfig::kDefaultContextWindow, 8192, LlmConfig::kSaneCeilingCtx),
                 LlmConfig::kSaneCeilingCtx);
        QCOMPARE(
            effectiveContextWindow(
                131072, LlmConfig::kDefaultContextWindow, 16384, LlmConfig::kSaneCeilingCtxHeavy),
            LlmConfig::kSaneCeilingCtxHeavy);
        QCOMPARE(effectiveContextWindow(
                     2048, LlmConfig::kDefaultContextWindow, 8192, LlmConfig::kSaneCeilingCtxHeavy),
                 LlmConfig::kSaneFloorCtx);
        QCOMPARE(
            effectiveContextWindow(
                12000, LlmConfig::kDefaultContextWindow, 8192, LlmConfig::kSaneCeilingCtxHeavy),
            12000);
    }

    void test_Policy_UnknownKeepsCurrentFloor() {
        QCOMPARE(effectiveContextWindow(
                     0, LlmConfig::kDefaultContextWindow, 16384, LlmConfig::kSaneCeilingCtxHeavy),
                 16384);
        QCOMPARE(effectiveContextWindow(0,
                                        LlmConfig::kDefaultContextWindow,
                                        LlmConfig::kDefaultContextWindow,
                                        LlmConfig::kSaneCeilingCtx),
                 LlmConfig::kDefaultContextWindow);
    }
};

QTEST_APPLESS_MAIN(TestContextWindow)
#include "test-context-window.moc"
