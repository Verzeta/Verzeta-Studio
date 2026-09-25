// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/llm-config.h"

#include <QtTest>

#include <QJsonObject>

class TestLlmConfig : public QObject {
    Q_OBJECT

  private slots:
    void test_ragEnabledRoundTrip() {
        LlmConfig on;
        on.ragEnabled = true;
        QVERIFY(LlmConfig::fromJson(on.toJson()).ragEnabled == true);

        LlmConfig off;
        off.ragEnabled = false;
        QVERIFY(LlmConfig::fromJson(off.toJson()).ragEnabled == false);
    }

    void test_ragEnabledDefaultsFalseWhenAbsent() {
        QJsonObject legacy;
        legacy[QStringLiteral("provider_id")] = QStringLiteral("ollama");
        legacy[QStringLiteral("model_name")] = QStringLiteral("qwen3.5:9b");
        QCOMPARE(LlmConfig::fromJson(legacy).ragEnabled, false);
    }

    void test_ragEnabledPreservesOtherFields() {
        LlmConfig cfg;
        cfg.providerId = QStringLiteral("ollama");
        cfg.modelName = QStringLiteral("qwen3.5:9b");
        cfg.toolsEnabled = false;
        cfg.thinkingMode = true;
        cfg.dynamicCompactEnabled = false;
        cfg.ragEnabled = true;
        cfg.implicitTaskCompletion = true;

        const LlmConfig out = LlmConfig::fromJson(cfg.toJson());
        QCOMPARE(out.ragEnabled, true);
        QCOMPARE(out.toolsEnabled, false);
        QCOMPARE(out.thinkingMode, true);
        QCOMPARE(out.dynamicCompactEnabled, false);
        QCOMPARE(out.implicitTaskCompletion, true);
        QCOMPARE(LlmConfig::fromJson(QJsonObject{}).implicitTaskCompletion, false);
        QCOMPARE(out.providerId, QStringLiteral("ollama"));
        QCOMPARE(out.modelName, QStringLiteral("qwen3.5:9b"));
    }

    void test_maxAutoRoundsRoundTrip() {
        LlmConfig a;
        a.maxAutoRounds = 12;
        QCOMPARE(LlmConfig::fromJson(a.toJson()).maxAutoRounds, 12);

        LlmConfig unbounded;
        unbounded.maxAutoRounds = 0;
        QCOMPARE(LlmConfig::fromJson(unbounded.toJson()).maxAutoRounds, 0);

        QJsonObject legacy;
        legacy[QStringLiteral("provider_id")] = QStringLiteral("ollama");
        QCOMPARE(LlmConfig::fromJson(legacy).maxAutoRounds, 6);
    }

    void test_aimEnabledRoundTrip() {
        LlmConfig on;
        on.aimEnabled = true;
        QVERIFY(LlmConfig::fromJson(on.toJson()).aimEnabled == true);

        LlmConfig off;
        off.aimEnabled = false;
        QVERIFY(LlmConfig::fromJson(off.toJson()).aimEnabled == false);
    }

    void test_aimEnabledDefaultsTrueWhenAbsent() {
        QJsonObject legacy;
        legacy[QStringLiteral("provider_id")] = QStringLiteral("ollama");
        QCOMPARE(LlmConfig::fromJson(legacy).aimEnabled, true);
    }

    void test_acnEnabledRoundTrip() {
        LlmConfig on;
        on.acnEnabled = true;
        QVERIFY(LlmConfig::fromJson(on.toJson()).acnEnabled == true);
        LlmConfig off;
        off.acnEnabled = false;
        QVERIFY(LlmConfig::fromJson(off.toJson()).acnEnabled == false);

        QJsonObject legacy;
        legacy[QStringLiteral("provider_id")] = QStringLiteral("ollama");
        QCOMPARE(LlmConfig::fromJson(legacy).acnEnabled, true);
    }
};

QTEST_APPLESS_MAIN(TestLlmConfig)
#include "test-llm-config.moc"
