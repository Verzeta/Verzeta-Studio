// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/db-manager.h"
#include "models/llm-config.h"
#include "remote/server/wire-db-reader.h"
#include "services/conversation-service.h"

#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <QDir>
#include <QFileInfo>
#include <QSqlDatabase>
#include <QStandardPaths>

class TestWireDbReaderLlmConfig : public QObject {
    Q_OBJECT

  private:
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<Verzeta::Remote::WireDbReader> m_reader;

  private slots:
    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void cleanupTestCase() { QStandardPaths::setTestModeEnabled(false); }

    void init() {
        const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QVERIFY(!dataDir.isEmpty());
        QDir().mkpath(dataDir);
        const QString dbPath = dataDir + QStringLiteral("/verzeta-studio.db");
        QFile::remove(dbPath);

        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));

        QVERIFY(DbManager::instance().open(dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_convSvc = std::make_unique<ConversationService>(DbManager::instance());

        m_reader = std::make_unique<Verzeta::Remote::WireDbReader>();
        QVERIFY(m_reader->open());
    }

    void cleanup() {
        m_reader.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    }

    void test_emptyConvId_returnsEmpty() {
        const QJsonObject result = m_reader->conversationSettings(QString());
        QVERIFY(result.isEmpty());
    }

    void test_unknownConvId_returnsEmpty() {
        const QJsonObject result =
            m_reader->conversationSettings(QStringLiteral("11111111-2222-3333-4444-555555555555"));
        QVERIFY(result.isEmpty());
    }

    void test_defaultLlmConfig_roundtrips() {
        const QString convId = m_convSvc->createConversation(QStringLiteral("default-config conv"));
        QVERIFY(!convId.isEmpty());

        const QJsonObject r = m_reader->conversationSettings(convId);
        QVERIFY(!r.isEmpty());
        QVERIFY(r.contains(QStringLiteral("systemPrompt")));
        QVERIFY(r.contains(QStringLiteral("isGroup")));
        QVERIFY(r.contains(QStringLiteral("folderId")));
        QVERIFY(!r.contains(QStringLiteral("providerId")));
        QVERIFY(!r.contains(QStringLiteral("modelName")));
        QVERIFY(!r.contains(QStringLiteral("maxTokens")));
        QVERIFY(!r.contains(QStringLiteral("contextWindow")));
        QVERIFY(!r.contains(QStringLiteral("streaming")));
        QVERIFY(!r.contains(QStringLiteral("thinking")));
        QVERIFY(!r.contains(QStringLiteral("agentPattern")));
        QVERIFY(!r.contains(QStringLiteral("toolsEnabled")));
        QVERIFY(!r.contains(QStringLiteral("requireConfirmation")));
    }

    void test_fullLlmConfig_roundtripsEveryField() {
        const QString convId = m_convSvc->createConversation(QStringLiteral("full-config conv"));
        QVERIFY(!convId.isEmpty());

        LlmConfig cfg;
        cfg.providerId = QStringLiteral("openai");
        cfg.modelName = QStringLiteral("gpt-4o-mini");
        cfg.temperature = 0.42;
        cfg.maxTokens = 8192;
        cfg.contextWindow = 32768;
        cfg.stream = false;
        cfg.thinkingMode = true;
        cfg.agentPattern = QStringLiteral("react");
        cfg.toolsEnabled = false;
        cfg.requireConfirmation = true;
        cfg.topK = 10;
        cfg.topP = 0.5;
        cfg.repeatPenalty = 1.03;
        cfg.presencePenalty = 0.25;
        cfg.frequencyPenalty = 0.5;
        cfg.forceAppSampling = false;
        cfg.toolsInSystemPrompt = true;
        cfg.dynamicCompactEnabled = false;
        cfg.compactEveryTurns = 35;

        QVERIFY(m_convSvc->updateLlmConfig(convId, cfg));

        const QJsonObject r = m_reader->conversationSettings(convId);
        QVERIFY(!r.isEmpty());

        QCOMPARE(r.value(QStringLiteral("providerId")).toString(), QStringLiteral("openai"));
        QCOMPARE(r.value(QStringLiteral("modelName")).toString(), QStringLiteral("gpt-4o-mini"));
        QCOMPARE(r.value(QStringLiteral("temperature")).toDouble(), 0.42);
        QCOMPARE(r.value(QStringLiteral("maxTokens")).toInt(), 8192);
        QCOMPARE(r.value(QStringLiteral("contextWindow")).toInt(), 32768);
        QCOMPARE(r.value(QStringLiteral("streaming")).toBool(), false);
        QCOMPARE(r.value(QStringLiteral("thinking")).toBool(), true);

        QCOMPARE(r.value(QStringLiteral("agentPattern")).toString(), QStringLiteral("react"));
        QCOMPARE(r.value(QStringLiteral("toolsEnabled")).toBool(), false);
        QCOMPARE(r.value(QStringLiteral("requireConfirmation")).toBool(), true);

        QCOMPARE(r.value(QStringLiteral("topK")).toDouble(), 10.0);
        QCOMPARE(r.value(QStringLiteral("topP")).toDouble(), 0.5);
        QCOMPARE(r.value(QStringLiteral("repeatPenalty")).toDouble(), 1.03);
        QCOMPARE(r.value(QStringLiteral("presencePenalty")).toDouble(), 0.25);
        QCOMPARE(r.value(QStringLiteral("frequencyPenalty")).toDouble(), 0.5);
        QCOMPARE(r.value(QStringLiteral("forceAppSampling")).toBool(), false);
        QCOMPARE(r.value(QStringLiteral("toolsInSystemPrompt")).toBool(), true);
        QCOMPARE(r.value(QStringLiteral("dynamicCompactEnabled")).toBool(), false);
        QCOMPARE(r.value(QStringLiteral("compactEveryTurns")).toInt(), 35);
    }

    void test_partialLlmConfig_onlyEmitsPresentFields() {
        const QString convId = m_convSvc->createConversation(QStringLiteral("partial-config conv"));
        QVERIFY(!convId.isEmpty());

        LlmConfig cfg;
        cfg.providerId = QStringLiteral("ollama");
        cfg.modelName = QStringLiteral("qwen3.5:9b");
        QVERIFY(m_convSvc->updateLlmConfig(convId, cfg));

        const QJsonObject r = m_reader->conversationSettings(convId);
        QCOMPARE(r.value(QStringLiteral("providerId")).toString(), QStringLiteral("ollama"));
        QCOMPARE(r.value(QStringLiteral("modelName")).toString(), QStringLiteral("qwen3.5:9b"));
        QCOMPARE(r.value(QStringLiteral("maxTokens")).toInt(), -1);
        QCOMPARE(r.value(QStringLiteral("contextWindow")).toInt(), 8192);
        QCOMPARE(r.value(QStringLiteral("streaming")).toBool(), true);
        QCOMPARE(r.value(QStringLiteral("thinking")).toBool(), false);
        QCOMPARE(r.value(QStringLiteral("agentPattern")).toString(), QString());
        QCOMPARE(r.value(QStringLiteral("toolsEnabled")).toBool(), true);
        QCOMPARE(r.value(QStringLiteral("requireConfirmation")).toBool(), false);
    }
};

QTEST_MAIN(TestWireDbReaderLlmConfig)
#include "test-wire-db-reader-llm-config.moc"
