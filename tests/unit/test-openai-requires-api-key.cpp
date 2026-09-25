// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/api/llamacpp-remote-provider.h"
#include "../../backend/api/openai-provider.h"
#include "../../backend/utils/http-client.h"

#include <QTest>

class TestOpenAIRequiresApiKey : public QObject {
    Q_OBJECT

  private slots:

    void test_openAIProvider_default_returns_true() {
        HttpClient http;
        OpenAIProvider provider(http);
        QVERIFY(provider.requiresApiKey());
    }

    void test_openAIProvider_with_key_still_returns_true() {
        HttpClient http;
        OpenAIProvider provider(http);
        provider.setApiKey(QStringLiteral("sk-some-real-key"));
        QVERIFY(provider.requiresApiKey());
    }

    void test_llamaCppRemoteProvider_no_key_returns_false() {
        HttpClient http;
        LlamaCppRemoteProvider provider(http);
        QVERIFY(!provider.requiresApiKey());
    }

    void test_llamaCppRemoteProvider_with_key_returns_true() {
        HttpClient http;
        LlamaCppRemoteProvider provider(http);
        provider.setApiKey(QStringLiteral("proxy-auth-token"));
        QVERIFY(provider.requiresApiKey());
    }
};

QTEST_MAIN(TestOpenAIRequiresApiKey)
#include "test-openai-requires-api-key.moc"
