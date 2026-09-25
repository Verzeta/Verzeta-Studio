// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/llm-config.h"
#include "services/model-sampling-profile.h"

#include <QTest>

using Verzeta::Models::applyProfileTo;
using Verzeta::Models::effectiveProfileWarning;
using Verzeta::Models::ModelSamplingProfile;
using Verzeta::Models::ModelSamplingProfileRegistry;

class TestModelSamplingProfile : public QObject {
    Q_OBJECT

  private slots:

    void emptyProviderReturnsNull() {
        const auto& r = ModelSamplingProfileRegistry::instance();
        QCOMPARE(r.find(QString(), QStringLiteral("qwen3.5:9b")),
                 static_cast<const ModelSamplingProfile*>(nullptr));
    }

    void emptyModelReturnsNull() {
        const auto& r = ModelSamplingProfileRegistry::instance();
        QCOMPARE(r.find(QStringLiteral("ollama"), QString()),
                 static_cast<const ModelSamplingProfile*>(nullptr));
    }

    void unknownPairReturnsNull() {
        const auto& r = ModelSamplingProfileRegistry::instance();
        QCOMPARE(r.find(QStringLiteral("ollama"), QStringLiteral("gemma4:e4b")),
                 static_cast<const ModelSamplingProfile*>(nullptr));
        QCOMPARE(r.find(QStringLiteral("openai"), QStringLiteral("gpt-4o")),
                 static_cast<const ModelSamplingProfile*>(nullptr));
        QCOMPARE(r.find(QStringLiteral("anthropic"), QStringLiteral("claude-3-5-sonnet-20241022")),
                 static_cast<const ModelSamplingProfile*>(nullptr));
    }


    void qwen35NinebOnOllamaMatches() {
        const auto& r = ModelSamplingProfileRegistry::instance();
        const ModelSamplingProfile* p =
            r.find(QStringLiteral("ollama"), QStringLiteral("qwen3.5:9b"));
        QVERIFY(p != nullptr);
        QCOMPARE(p->topK.value_or(-1), 10);
        QCOMPARE(p->temperature.value_or(-1), 0.6);
        QCOMPARE(p->topP.value_or(-1), 0.5);
        QCOMPARE(p->repeatPenalty.value_or(-1), 1.03);
        QCOMPARE(p->presencePenalty.value_or(-1), 0.0);
        QCOMPARE(p->frequencyPenalty.value_or(-1), 0.0);
        QVERIFY(!p->reason.isEmpty());
        QVERIFY(!p->warning.isEmpty());
    }

    void qwen35FourteenbOnOllamaMatchesViaWildcard() {
        const auto& r = ModelSamplingProfileRegistry::instance();
        const ModelSamplingProfile* p =
            r.find(QStringLiteral("ollama"), QStringLiteral("qwen3.5:14b"));
        QVERIFY(p != nullptr);
        QCOMPARE(p->topK.value_or(-1), 10);
    }

    void qwen35WithoutTagSuffixMatches() {
        const auto& r = ModelSamplingProfileRegistry::instance();
        const ModelSamplingProfile* p =
            r.find(QStringLiteral("ollama"), QStringLiteral("qwen3.5:9b-instruct-q4_K_M"));
        QVERIFY(p != nullptr);
    }


    void qwen36OnOllamaMatches() {
        const auto& r = ModelSamplingProfileRegistry::instance();
        const ModelSamplingProfile* p =
            r.find(QStringLiteral("ollama"), QStringLiteral("qwen3.6:35b-a3b-q4_K_M"));
        QVERIFY(p != nullptr);
        QCOMPARE(p->topK.value_or(-1), 10);
    }


    void caseInsensitiveProviderAndModel() {
        const auto& r = ModelSamplingProfileRegistry::instance();
        const ModelSamplingProfile* p1 =
            r.find(QStringLiteral("Ollama"), QStringLiteral("Qwen3.5:9B"));
        QVERIFY(p1 != nullptr);
        const ModelSamplingProfile* p2 =
            r.find(QStringLiteral("OLLAMA"), QStringLiteral("QWEN3.5:9B"));
        QVERIFY(p2 != nullptr);
    }


    void qwen35OnLlamaCppDoesNotMatch() {
        const auto& r = ModelSamplingProfileRegistry::instance();
        const ModelSamplingProfile* p =
            r.find(QStringLiteral("llamacpp"), QStringLiteral("qwen3.5:9b"));
        QCOMPARE(p, static_cast<const ModelSamplingProfile*>(nullptr));
    }

    void qwen35OnOpenAICompatDoesNotMatch() {
        const auto& r = ModelSamplingProfileRegistry::instance();
        const ModelSamplingProfile* p =
            r.find(QStringLiteral("openai_compat"), QStringLiteral("qwen3.5:9b"));
        QCOMPARE(p, static_cast<const ModelSamplingProfile*>(nullptr));
    }


    void qwen36ProfileDoesNotMatchQwen37() {
        const auto& r = ModelSamplingProfileRegistry::instance();
        const ModelSamplingProfile* p =
            r.find(QStringLiteral("ollama"), QStringLiteral("qwen3.7:9b"));
        QCOMPARE(p, static_cast<const ModelSamplingProfile*>(nullptr));
    }

    void qwen35PrefixDoesNotSwallowQwen3() {
        const auto& r = ModelSamplingProfileRegistry::instance();
        const ModelSamplingProfile* p =
            r.find(QStringLiteral("ollama"), QStringLiteral("qwen3:9b"));
        QCOMPARE(p, static_cast<const ModelSamplingProfile*>(nullptr));
    }


    void singletonReturnsSameInstance() {
        const auto& r1 = ModelSamplingProfileRegistry::instance();
        const auto& r2 = ModelSamplingProfileRegistry::instance();
        QCOMPARE(&r1, &r2);
    }

    void profilePointerStableAcrossCalls() {
        const auto& r = ModelSamplingProfileRegistry::instance();
        const ModelSamplingProfile* p1 =
            r.find(QStringLiteral("ollama"), QStringLiteral("qwen3.5:9b"));
        const ModelSamplingProfile* p2 =
            r.find(QStringLiteral("ollama"), QStringLiteral("qwen3.5:9b"));
        QVERIFY(p1 != nullptr);
        QCOMPARE(p1, p2);
    }


    void catalogueHasAtLeastTheTwoQwenEntries() {
        const auto& r = ModelSamplingProfileRegistry::instance();
        QVERIFY(r.catalogueSize() >= 2);
    }


    void applyProfileTo_qwen35Ollama_forceMode_overridesAll() {
        LlmConfig cfg;
        cfg.providerId = QStringLiteral("ollama");
        cfg.modelName = QStringLiteral("qwen3.5:9b");
        cfg.forceAppSampling = true;

        const ModelSamplingProfile* p = applyProfileTo(cfg);
        QVERIFY(p != nullptr);

        QCOMPARE(cfg.temperature, 0.6);
        QCOMPARE(cfg.topK, 10.0);
        QCOMPARE(cfg.topP, 0.5);
        QCOMPARE(cfg.repeatPenalty, 1.03);
        QCOMPARE(cfg.presencePenalty, 0.0);
        QCOMPARE(cfg.frequencyPenalty, 0.0);
    }

    void applyProfileTo_qwen35Ollama_forceMode_overridesUserValues() {
        LlmConfig cfg;
        cfg.providerId = QStringLiteral("ollama");
        cfg.modelName = QStringLiteral("qwen3.5:9b");
        cfg.forceAppSampling = true;
        cfg.temperature = 1.0;
        cfg.topK = 40;
        cfg.topP = 0.9;

        applyProfileTo(cfg);

        QCOMPARE(cfg.temperature, 0.6);
        QCOMPARE(cfg.topK, 10.0);
        QCOMPARE(cfg.topP, 0.5);
    }

    void applyProfileTo_qwen35Ollama_additiveMode_fillsUnsetOnly() {
        LlmConfig cfg;
        cfg.providerId = QStringLiteral("ollama");
        cfg.modelName = QStringLiteral("qwen3.5:9b");
        cfg.forceAppSampling = false;
        cfg.topK = 40;

        applyProfileTo(cfg);

        QCOMPARE(cfg.topK, 40.0);
        QCOMPARE(cfg.temperature, 0.6);
        QCOMPARE(cfg.topP, 0.5);
    }

    void applyProfileTo_additiveMode_userTemperatureSet_keepsUserValue() {
        LlmConfig cfg;
        cfg.providerId = QStringLiteral("ollama");
        cfg.modelName = QStringLiteral("qwen3.5:9b");
        cfg.forceAppSampling = false;
        cfg.temperature = 0.7;

        applyProfileTo(cfg);

        QCOMPARE(cfg.temperature, 0.7);
        QCOMPARE(cfg.topK, 10.0);
    }

    void applyProfileTo_noMatchingProfile_leavesCfgUntouched() {
        LlmConfig cfg;
        cfg.providerId = QStringLiteral("ollama");
        cfg.modelName = QStringLiteral("gemma4:e4b");
        const auto before_temp = cfg.temperature;
        const auto before_topK = cfg.topK;

        const ModelSamplingProfile* p = applyProfileTo(cfg);
        QCOMPARE(p, static_cast<const ModelSamplingProfile*>(nullptr));

        QCOMPARE(cfg.temperature, before_temp);
        QCOMPARE(cfg.topK, before_topK);
    }

    void applyProfileTo_qwen35LlamaCpp_noProfile() {
        LlmConfig cfg;
        cfg.providerId = QStringLiteral("llamacpp");
        cfg.modelName = QStringLiteral("qwen3.5:9b");

        const ModelSamplingProfile* p = applyProfileTo(cfg);
        QCOMPARE(p, static_cast<const ModelSamplingProfile*>(nullptr));
        QCOMPARE(cfg.temperature, -1.0);
        QCOMPARE(cfg.topK, -1.0);
    }


    void effectiveProfileWarning_qwen35OllamaNoOverride_returnsWarning() {
        LlmConfig cfg;
        cfg.providerId = QStringLiteral("ollama");
        cfg.modelName = QStringLiteral("qwen3.5:9b");

        const QString w = effectiveProfileWarning(cfg);
        QVERIFY(!w.isEmpty());
    }

    void effectiveProfileWarning_noMatchingProfile_returnsEmpty() {
        LlmConfig cfg;
        cfg.providerId = QStringLiteral("ollama");
        cfg.modelName = QStringLiteral("gemma4:e4b");

        const QString w = effectiveProfileWarning(cfg);
        QVERIFY(w.isEmpty());
    }

    void effectiveProfileWarning_emptyConfig_returnsEmpty() {
        LlmConfig cfg;
        const QString w = effectiveProfileWarning(cfg);
        QVERIFY(w.isEmpty());
    }
};

QTEST_MAIN(TestModelSamplingProfile)
#include "test-model-sampling-profile.moc"
