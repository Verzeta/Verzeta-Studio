// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/api/openai-compat-provider.h"
#include "../../backend/utils/http-client.h"

#include <QTest>

class TestOpenAICompatProvider : public QObject {
    Q_OBJECT

  private slots:


    void test_providerId_empty_when_slug_unset() {
        HttpClient http;
        OpenAICompatProvider provider(http);
        QCOMPARE(provider.providerId(), QString());
    }

    void test_providerId_composes_custom_namespace() {
        HttpClient http;
        OpenAICompatProvider provider(http);
        provider.setSlug(QStringLiteral("lm-studio-home"));
        QCOMPARE(provider.providerId(), QStringLiteral("custom.lm-studio-home"));
    }

    void test_providerId_updates_on_slug_change() {
        HttpClient http;
        OpenAICompatProvider provider(http);
        provider.setSlug(QStringLiteral("first"));
        QCOMPARE(provider.providerId(), QStringLiteral("custom.first"));
        provider.setSlug(QStringLiteral("second"));
        QCOMPARE(provider.providerId(), QStringLiteral("custom.second"));
    }

    void test_slug_roundtrips() {
        HttpClient http;
        OpenAICompatProvider provider(http);
        provider.setSlug(QStringLiteral("vllm-lab"));
        QCOMPARE(provider.slug(), QStringLiteral("vllm-lab"));
    }

    void test_displayName_roundtrips() {
        HttpClient http;
        OpenAICompatProvider provider(http);
        provider.setDisplayName(QStringLiteral("LM Studio at home"));
        QCOMPARE(provider.displayName(), QStringLiteral("LM Studio at home"));
    }


    void test_supportsStreaming_default_true() {
        HttpClient http;
        OpenAICompatProvider provider(http);
        QVERIFY(provider.supportsStreaming());
    }

    void test_supportsToolCalling_default_true() {
        HttpClient http;
        OpenAICompatProvider provider(http);
        QVERIFY(provider.supportsToolCalling());
    }

    void test_supportsVision_default_false() {
        HttpClient http;
        OpenAICompatProvider provider(http);
        QVERIFY(!provider.supportsVision());
    }

    void test_requiresApiKeyFlag_default_false() {
        HttpClient http;
        OpenAICompatProvider provider(http);
        QVERIFY(!provider.requiresApiKeyFlag());
    }


    void test_supportsStreaming_roundtrip_false() {
        HttpClient http;
        OpenAICompatProvider provider(http);
        provider.setSupportsStreamingFlag(false);
        QVERIFY(!provider.supportsStreaming());
        provider.setSupportsStreamingFlag(true);
        QVERIFY(provider.supportsStreaming());
    }

    void test_supportsToolCalling_roundtrip_false() {
        HttpClient http;
        OpenAICompatProvider provider(http);
        provider.setSupportsToolCallingFlag(false);
        QVERIFY(!provider.supportsToolCalling());
        provider.setSupportsToolCallingFlag(true);
        QVERIFY(provider.supportsToolCalling());
    }

    void test_supportsVision_roundtrip_true() {
        HttpClient http;
        OpenAICompatProvider provider(http);
        provider.setSupportsVisionFlag(true);
        QVERIFY(provider.supportsVision());
        provider.setSupportsVisionFlag(false);
        QVERIFY(!provider.supportsVision());
    }


    void test_requiresApiKey_flagOff_emptyKey_false() {
        HttpClient http;
        OpenAICompatProvider provider(http);
        QVERIFY(!provider.requiresApiKey());
    }

    void test_requiresApiKey_flagOff_keySet_true() {
        HttpClient http;
        OpenAICompatProvider provider(http);
        provider.setApiKey(QStringLiteral("sk-test-value"));
        QVERIFY(provider.requiresApiKey());
    }

    void test_requiresApiKey_flagOn_emptyKey_true() {
        HttpClient http;
        OpenAICompatProvider provider(http);
        provider.setRequiresApiKeyFlag(true);
        QVERIFY(provider.requiresApiKey());
    }

    void test_requiresApiKey_flagOn_keySet_true() {
        HttpClient http;
        OpenAICompatProvider provider(http);
        provider.setRequiresApiKeyFlag(true);
        provider.setApiKey(QStringLiteral("proxy-token"));
        QVERIFY(provider.requiresApiKey());
    }

    void test_requiresApiKey_clearKey_flagOff_returnsToKeyless() {
        HttpClient http;
        OpenAICompatProvider provider(http);
        provider.setApiKey(QStringLiteral("some-key"));
        QVERIFY(provider.requiresApiKey());
        provider.setApiKey(QString());
        QVERIFY(!provider.requiresApiKey());
    }
};

QTEST_MAIN(TestOpenAICompatProvider)
#include "test-openai-compat-provider.moc"
