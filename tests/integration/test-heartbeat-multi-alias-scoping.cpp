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
#include <QSet>
#include <QSqlDatabase>

class TestHeartbeatMultiAliasScoping : public QObject {
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
        a.id = QStringLiteral("agent-Researcher");
        a.name = QStringLiteral("TestResearcher-29H1");
        a.systemPrompt = QStringLiteral("You research.");
        a.createdAt = QDateTime::currentDateTimeUtc();
        QVERIFY(!m_agents->createAgent(a).isEmpty());

        m_groupConvId = m_convs->createConversation(QStringLiteral("Research Team"));
        QVERIFY(!m_groupConvId.isEmpty());

        for (int i = 0; i < 4; ++i) {
            HeartbeatConfig cfg;
            cfg.id = QStringLiteral("hb-r-%1").arg(i);
            cfg.agentId = QStringLiteral("agent-Researcher");
            cfg.scopeType = HeartbeatScopeType::ConversationGroup;
            cfg.scopeId = m_groupConvId;
            cfg.alias = m_aliases[i];
            cfg.enabled = true;
            cfg.schedule = QStringLiteral("@interval 15");
            cfg.goal = QStringLiteral("monitor %1").arg(m_aliases[i]);
            cfg.maxRunsPerDay = 24;
            QVERIFY2(!m_svc->upsertConfig(cfg).isEmpty(),
                     qPrintable(QStringLiteral("upsert failed for ") + cfg.alias));
        }
    }

    void cleanup() {
        m_svc.reset();
        m_convs.reset();
        m_agents.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QFile::remove(m_dbPath);
    }

    void test_fourAliasesAllPersist() {
        const auto rows =
            m_svc->configsInScope(HeartbeatScopeType::ConversationGroup, m_groupConvId);
        QCOMPARE(rows.size(), 4);

        QSet<QString> seen;
        for (const auto& cfg : rows) {
            QVERIFY(!cfg.alias.isEmpty());
            seen.insert(cfg.alias);
            QCOMPARE(cfg.agentId, QStringLiteral("agent-Researcher"));
            QCOMPARE(cfg.scopeType, HeartbeatScopeType::ConversationGroup);
            QCOMPARE(cfg.scopeId, m_groupConvId);
        }
        QCOMPARE(seen.size(), 4);
        for (const auto& alias : m_aliases) {
            QVERIFY(seen.contains(alias));
        }
    }

    void test_configForResolvesEachAliasIndependently() {
        QSet<QString> ids;
        for (int i = 0; i < 4; ++i) {
            const auto cfg = m_svc->configFor(QStringLiteral("agent-Researcher"),
                                              HeartbeatScopeType::ConversationGroup,
                                              m_groupConvId,
                                              m_aliases[i]);
            QVERIFY2(!cfg.id.isEmpty(),
                     qPrintable(QStringLiteral("missing config for ") + m_aliases[i]));
            QCOMPARE(cfg.alias, m_aliases[i]);
            ids.insert(cfg.id);
        }
        QCOMPARE(ids.size(), 4);
    }

    void test_updateLastFireIndependentAcrossAliases() {
        const QString tickAlpha = QStringLiteral("2026-05-02T11:00:00.000");
        const auto alphaCfg = m_svc->configFor(QStringLiteral("agent-Researcher"),
                                               HeartbeatScopeType::ConversationGroup,
                                               m_groupConvId,
                                               m_aliases[0]);
        QVERIFY(!alphaCfg.id.isEmpty());

        QVERIFY(m_svc->updateLastFire(alphaCfg.id, tickAlpha, QStringLiteral("success")));

        const auto alphaAfter = m_svc->configById(alphaCfg.id);
        QCOMPARE(alphaAfter.lastFireAt, tickAlpha);

        for (int i = 1; i < 4; ++i) {
            const auto cfg = m_svc->configFor(QStringLiteral("agent-Researcher"),
                                              HeartbeatScopeType::ConversationGroup,
                                              m_groupConvId,
                                              m_aliases[i]);
            QVERIFY(cfg.lastFireAt.isEmpty());
        }
    }

    void test_duplicateAliasOnSameScopeRejected() {
        HeartbeatConfig dup;
        dup.id = QStringLiteral("hb-dup");
        dup.agentId = QStringLiteral("agent-Researcher");
        dup.scopeType = HeartbeatScopeType::ConversationGroup;
        dup.scopeId = m_groupConvId;
        dup.alias = m_aliases[0];
        QVERIFY(m_svc->upsertConfig(dup).isEmpty());
    }

  private:
    QTemporaryDir m_tempDir;
    QString m_dbPath;
    QString m_groupConvId;
    std::unique_ptr<AgentRegistry> m_agents;
    std::unique_ptr<ConversationService> m_convs;
    std::unique_ptr<HeartbeatConfigService> m_svc;

    const QString m_aliases[4] = {
        QStringLiteral("ResearcherNorthAmerica"),
        QStringLiteral("ResearcherTikTok"),
        QStringLiteral("ResearcherCompetitors"),
        QStringLiteral("ResearcherSEO"),
    };
};

QTEST_MAIN(TestHeartbeatMultiAliasScoping)
#include "test-heartbeat-multi-alias-scoping.moc"
