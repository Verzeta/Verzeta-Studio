// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/agent.h"
#include "models/db-manager.h"
#include "models/heartbeat-config.h"
#include "services/agent-registry.h"
#include "services/conversation-service.h"
#include "services/heartbeat-config-service.h"

#include <QTemporaryDir>
#include <QtTest>

#include <QDateTime>
#include <QFile>
#include <QSqlDatabase>

class TestHeartbeatConfigService : public QObject {
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

        m_agents = std::make_unique<AgentRegistry>(DbManager::instance());
        m_agents->initialize();
        m_convs = std::make_unique<ConversationService>(DbManager::instance());
        m_svc = std::make_unique<HeartbeatConfigService>(DbManager::instance());

        Agent a;
        a.id = QStringLiteral("agent-test-1");
        a.name = QStringLiteral("Test Agent");
        a.systemPrompt = QStringLiteral("You are a test.");
        a.createdAt = QDateTime::currentDateTimeUtc();
        QVERIFY(!m_agents->createAgent(a).isEmpty());

        m_convId = m_convs->createConversation(QStringLiteral("Test Conv"));
        QVERIFY(!m_convId.isEmpty());
    }

    void cleanup() {
        m_svc.reset();
        m_convs.reset();
        m_agents.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QFile::remove(m_dbPath);
    }

    void test_insertAndLookupById() {
        HeartbeatConfig cfg;
        cfg.id = QStringLiteral("hb-001");
        cfg.agentId = QStringLiteral("agent-test-1");
        cfg.scopeType = HeartbeatScopeType::Conversation1to1;
        cfg.scopeId = m_convId;
        cfg.alias = QString();
        cfg.enabled = true;
        cfg.schedule = QStringLiteral("@interval 15");
        cfg.goal = QStringLiteral("monitor twitter");
        cfg.maxRunsPerDay = 24;

        QCOMPARE(m_svc->upsertConfig(cfg), QStringLiteral("hb-001"));

        const auto got = m_svc->configById(QStringLiteral("hb-001"));
        QCOMPARE(got.id, QStringLiteral("hb-001"));
        QCOMPARE(got.agentId, QStringLiteral("agent-test-1"));
        QCOMPARE(got.scopeType, HeartbeatScopeType::Conversation1to1);
        QCOMPARE(got.scopeId, m_convId);
        QVERIFY(got.alias.isEmpty());
        QCOMPARE(got.enabled, true);
        QCOMPARE(got.schedule, QStringLiteral("@interval 15"));
        QCOMPARE(got.goal, QStringLiteral("monitor twitter"));
    }

    void test_lookupByIdMissReturnsEmpty() {
        QVERIFY(m_svc->configById(QStringLiteral("nope")).id.isEmpty());
    }

    void test_configForTupleLookup() {
        HeartbeatConfig a;
        a.id = QStringLiteral("hb-tuple-1");
        a.agentId = QStringLiteral("agent-test-1");
        a.scopeType = HeartbeatScopeType::ConversationGroup;
        a.scopeId = m_convId;
        a.alias = QStringLiteral("Researcher");
        a.schedule = QStringLiteral("@hourly");
        QVERIFY(!m_svc->upsertConfig(a).isEmpty());

        const auto got = m_svc->configFor(QStringLiteral("agent-test-1"),
                                          HeartbeatScopeType::ConversationGroup,
                                          m_convId,
                                          QStringLiteral("Researcher"));
        QCOMPARE(got.id, QStringLiteral("hb-tuple-1"));

        const auto miss = m_svc->configFor(QStringLiteral("agent-test-1"),
                                           HeartbeatScopeType::ConversationGroup,
                                           m_convId,
                                           QStringLiteral("OtherAlias"));
        QVERIFY(miss.id.isEmpty());
    }

    void test_uniqueConstraintOnTuple() {
        HeartbeatConfig a;
        a.id = QStringLiteral("hb-u-1");
        a.agentId = QStringLiteral("agent-test-1");
        a.scopeType = HeartbeatScopeType::ConversationGroup;
        a.scopeId = m_convId;
        a.alias = QStringLiteral("Alice");
        QVERIFY(!m_svc->upsertConfig(a).isEmpty());

        HeartbeatConfig dup = a;
        dup.id = QStringLiteral("hb-u-2");
        QVERIFY(m_svc->upsertConfig(dup).isEmpty());

        HeartbeatConfig b = a;
        b.id = QStringLiteral("hb-u-3");
        b.alias = QStringLiteral("Bob");
        QVERIFY(!m_svc->upsertConfig(b).isEmpty());
    }

    void test_enabledConfigsOrdering() {
        HeartbeatConfig a;
        a.id = QStringLiteral("hb-en-1");
        a.agentId = QStringLiteral("agent-test-1");
        a.scopeType = HeartbeatScopeType::Conversation1to1;
        a.scopeId = m_convId;
        a.alias = QString();
        a.enabled = true;
        a.schedule = QStringLiteral("@hourly");
        a.lastFireAt = QStringLiteral("2026-05-02T10:00:00.000");
        QVERIFY(!m_svc->upsertConfig(a).isEmpty());

        HeartbeatConfig b = a;
        b.id = QStringLiteral("hb-en-2");
        b.alias = QStringLiteral("never");
        b.scopeType = HeartbeatScopeType::ConversationGroup;
        b.lastFireAt.clear();
        QVERIFY(!m_svc->upsertConfig(b).isEmpty());

        HeartbeatConfig c = a;
        c.id = QStringLiteral("hb-en-3");
        c.alias = QStringLiteral("disabled");
        c.scopeType = HeartbeatScopeType::ConversationGroup;
        c.enabled = false;
        QVERIFY(!m_svc->upsertConfig(c).isEmpty());

        const auto enabled = m_svc->enabledConfigs();
        QCOMPARE(enabled.size(), 2);
        QCOMPARE(enabled[0].id, QStringLiteral("hb-en-2"));
        QCOMPARE(enabled[1].id, QStringLiteral("hb-en-1"));
    }

    void test_updateLastFireAnchor() {
        HeartbeatConfig a;
        a.id = QStringLiteral("hb-lf-1");
        a.agentId = QStringLiteral("agent-test-1");
        a.scopeType = HeartbeatScopeType::Conversation1to1;
        a.scopeId = m_convId;
        a.alias = QString();
        a.schedule = QStringLiteral("@interval 5");
        QVERIFY(!m_svc->upsertConfig(a).isEmpty());

        const QString ts = QStringLiteral("2026-05-02T11:30:45.123");
        QVERIFY(m_svc->updateLastFire(a.id, ts, QStringLiteral("rate_limited")));

        const auto got = m_svc->configById(a.id);
        QCOMPARE(got.lastFireAt, ts);
        QCOMPARE(got.lastFireOutcome, QStringLiteral("rate_limited"));
    }

    void test_cascadeDeleteOnAgentRemoval() {
        HeartbeatConfig a;
        a.id = QStringLiteral("hb-cascade-1");
        a.agentId = QStringLiteral("agent-test-1");
        a.scopeType = HeartbeatScopeType::Conversation1to1;
        a.scopeId = m_convId;
        a.alias = QString();
        QVERIFY(!m_svc->upsertConfig(a).isEmpty());

        QVERIFY(!m_svc->configById(a.id).id.isEmpty());

        QVERIFY(m_agents->deleteAgent(QStringLiteral("agent-test-1")));

        QVERIFY(m_svc->configById(a.id).id.isEmpty());
    }

    void test_onConversationDeletedRemovesScopedRows() {
        HeartbeatConfig a;
        a.id = QStringLiteral("hb-conv-1");
        a.agentId = QStringLiteral("agent-test-1");
        a.scopeType = HeartbeatScopeType::Conversation1to1;
        a.scopeId = m_convId;
        a.alias = QString();
        QVERIFY(!m_svc->upsertConfig(a).isEmpty());

        const int dropped = m_svc->onConversationDeleted(m_convId);
        QCOMPARE(dropped, 1);
        QVERIFY(m_svc->configById(a.id).id.isEmpty());
    }

  private:
    QTemporaryDir m_tempDir;
    QString m_dbPath;
    std::unique_ptr<AgentRegistry> m_agents;
    std::unique_ptr<ConversationService> m_convs;
    std::unique_ptr<HeartbeatConfigService> m_svc;
    QString m_convId;
};

QTEST_MAIN(TestHeartbeatConfigService)
#include "test-heartbeat-config-service.moc"
