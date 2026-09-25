// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/conversation.h"
#include "models/db-manager.h"
#include "models/llm-config.h"
#include "services/conversation-service.h"

#include <QTemporaryDir>
#include <QtTest>

#include <QFile>
#include <QJsonObject>
#include <QSqlDatabase>

class TestHeartbeatAutoSurfaceGate : public QObject {
    Q_OBJECT

  private slots:
    void init() {
        QVERIFY(m_tempDir.isValid());
        const QString dbPath =
            m_tempDir.path() +
            QStringLiteral("/test_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
        m_dbPath = dbPath;
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_svc = std::make_unique<ConversationService>(DbManager::instance());

        m_convId = m_svc->createConversation(QStringLiteral("Test Chat"));
        QVERIFY(!m_convId.isEmpty());
    }

    void cleanup() {
        m_svc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QFile::remove(m_dbPath);
    }

    void test_defaultOffOnFreshConversation() {
        const auto conv = m_svc->getConversation(m_convId);
        QVERIFY(conv.has_value());
        QCOMPARE(conv->allowHeartbeatAutoSurface(), false);
        QCOMPARE(conv->heartbeatAutoSurfaceMaxPerDay(), 1);
    }

    void test_setterRoundTrips() {
        QVERIFY(m_svc->setHeartbeatAutoSurface(m_convId, true, 7));
        const auto conv = m_svc->getConversation(m_convId);
        QVERIFY(conv.has_value());
        QCOMPARE(conv->allowHeartbeatAutoSurface(), true);
        QCOMPARE(conv->heartbeatAutoSurfaceMaxPerDay(), 7);
    }

    void test_setterPreservesOtherLlmConfigFields() {
        LlmConfig cfg;
        cfg.providerId = QStringLiteral("openai");
        cfg.modelName = QStringLiteral("gpt-4o");
        cfg.temperature = 0.7;
        cfg.maxTokens = 8192;
        cfg.contextWindow = 32768;
        cfg.stream = false;
        cfg.thinkingMode = true;
        QVERIFY(m_svc->updateLlmConfig(m_convId, cfg));

        QVERIFY(m_svc->setHeartbeatAutoSurface(m_convId, true, 5));

        const auto conv = m_svc->getConversation(m_convId);
        QVERIFY(conv.has_value());
        const LlmConfig roundTripped = LlmConfig::fromJson(conv->llmConfig);
        QCOMPARE(roundTripped.providerId, QStringLiteral("openai"));
        QCOMPARE(roundTripped.modelName, QStringLiteral("gpt-4o"));
        QCOMPARE(roundTripped.temperature, 0.7);
        QCOMPARE(roundTripped.maxTokens, 8192);
        QCOMPARE(roundTripped.contextWindow, 32768);
        QCOMPARE(roundTripped.stream, false);
        QCOMPARE(roundTripped.thinkingMode, true);
        QCOMPARE(roundTripped.allowHeartbeatAutoSurface, true);
        QCOMPARE(roundTripped.autoSurfaceMaxPerDay, 5);
    }

    void test_updateLlmConfig_carriesGateFields() {
        QVERIFY(m_svc->setHeartbeatAutoSurface(m_convId, true, 10));

        auto conv = m_svc->getConversation(m_convId);
        QVERIFY(conv.has_value());
        LlmConfig roundTripped = LlmConfig::fromJson(conv->llmConfig);
        QCOMPARE(roundTripped.allowHeartbeatAutoSurface, true);
        QCOMPARE(roundTripped.autoSurfaceMaxPerDay, 10);

        QVERIFY(m_svc->updateLlmConfig(m_convId, roundTripped));

        conv = m_svc->getConversation(m_convId);
        QVERIFY(conv.has_value());
        QCOMPARE(conv->allowHeartbeatAutoSurface(), true);
        QCOMPARE(conv->heartbeatAutoSurfaceMaxPerDay(), 10);
    }

    void test_setterClampsCapFloorToOne() {
        QVERIFY(m_svc->setHeartbeatAutoSurface(m_convId, true, 0));
        const auto conv = m_svc->getConversation(m_convId);
        QVERIFY(conv.has_value());
        QCOMPARE(conv->heartbeatAutoSurfaceMaxPerDay(), 1);
    }

    void test_setterClampsCapFloorOnNegative() {
        QVERIFY(m_svc->setHeartbeatAutoSurface(m_convId, true, -5));
        const auto conv = m_svc->getConversation(m_convId);
        QVERIFY(conv.has_value());
        QCOMPARE(conv->heartbeatAutoSurfaceMaxPerDay(), 1);
    }

    void test_setterOnUnknownConvIdFailsCleanly() {
        QVERIFY(!m_svc->setHeartbeatAutoSurface(QStringLiteral("does-not-exist"), true, 5));
    }

  private:
    QTemporaryDir m_tempDir;
    QString m_dbPath;
    QString m_convId;
    std::unique_ptr<ConversationService> m_svc;
};

QTEST_MAIN(TestHeartbeatAutoSurfaceGate)
#include "test-heartbeat-auto-surface-gate.moc"
