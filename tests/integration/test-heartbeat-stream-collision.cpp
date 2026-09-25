// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/api/llm-interface.h"
#include "../../backend/models/agent.h"
#include "../../backend/models/db-manager.h"
#include "../../backend/models/heartbeat-config.h"
#include "../../backend/models/message.h"
#include "../../backend/services/agent-registry.h"
#include "../../backend/services/chat/request-builder.h"
#include "../../backend/services/conversation-service.h"
#include "../../backend/services/heartbeat-config-service.h"
#include "../../backend/services/heartbeat-subagent-service.h"
#include "../../backend/services/message-service.h"
#include "../../backend/services/model-router.h"
#include "../../backend/services/skill-service.h"
#include "../../backend/services/tool-service.h"

#include <QTemporaryDir>
#include <QtTest>

#include <QDateTime>
#include <QFile>
#include <QRandomGenerator>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>

class TestHeartbeatStreamCollision : public QObject {
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

        m_router = std::make_unique<ModelRouter>();
        m_toolSvc = std::make_unique<ToolService>();
        m_agents = std::make_unique<AgentRegistry>(DbManager::instance());
        m_agents->initialize();
        m_convs = std::make_unique<ConversationService>(DbManager::instance());
        m_skillSvc = std::make_unique<SkillService>(*m_convs);
        m_skillSvc->setAppDataRootForTesting(m_tempDir.path());
        m_skillSvc->initialize();
        m_msgs = std::make_unique<MessageService>(DbManager::instance());
        m_reqBuilder = std::make_unique<Chat::RequestBuilder>(*m_convs, *m_msgs, *m_router);
        m_configSvc = std::make_unique<HeartbeatConfigService>(DbManager::instance());

        Agent a;
        a.id = QStringLiteral("agent-sc");
        a.name = QStringLiteral("StreamCollisionAgent");
        a.systemPrompt = QStringLiteral("you are tester");
        a.createdAt = QDateTime::currentDateTimeUtc();
        QVERIFY(!m_agents->createAgent(a).isEmpty());
        m_convA = m_convs->createConversation(QStringLiteral("Conv A"));
        m_convB = m_convs->createConversation(QStringLiteral("Conv B"));
        QVERIFY(!m_convA.isEmpty());
        QVERIFY(!m_convB.isEmpty());

        m_svc = std::make_unique<HeartbeatSubagentService>(*m_configSvc,
                                                           *m_agents,
                                                           *m_convs,
                                                           *m_msgs,
                                                           *m_router,
                                                           *m_toolSvc,
                                                           *m_skillSvc,
                                                           *m_reqBuilder,
                                                           nullptr,
                                                           nullptr);

        seedConfig(QStringLiteral("hb-A"), m_convA);
        seedConfig(QStringLiteral("hb-B"), m_convB);
    }

    void cleanup() {
        m_svc.reset();
        m_configSvc.reset();
        m_reqBuilder.reset();
        m_msgs.reset();
        m_convs.reset();
        m_agents.reset();
        m_skillSvc.reset();
        m_toolSvc.reset();
        m_router.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main'"));
        QFile::remove(m_dbPath);
    }

    void test_drainsBucketForResolvedConvId() {
        const QString reportId = seedSkippedReport(QStringLiteral("hb-A"));

        const QString fgMsgId = persistDummyAssistantMessage(m_convA);

        m_svc->enqueuePendingPostForTest(m_convA,
                                         reportId,
                                         QStringLiteral("deferred body — should land after drain"),
                                         QStringLiteral("manual"));
        QCOMPARE(m_svc->pendingPostCountForTest(m_convA), 1);

        m_svc->invokeStreamFinalizedForTest(fgMsgId, QStringLiteral("stop"), true);

        QCOMPARE(m_svc->pendingPostCountForTest(m_convA), 0);

        bool foundDeferred = false;
        for (const auto& m : m_msgs->getMessages(m_convA)) {
            if (m.role == QStringLiteral("assistant") &&
                m.content.contains(QStringLiteral("deferred body"))) {
                foundDeferred = true;
                break;
            }
        }
        QVERIFY(foundDeferred);

        QSqlQuery q(DbManager::instance().db());
        q.prepare(QStringLiteral("SELECT surface_status FROM heartbeat_reports WHERE id = ?"));
        q.addBindValue(reportId);
        QVERIFY(q.exec());
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toString(), QStringLiteral("posted_manual"));
    }

    void test_unknownMsgId_doesNotDrain() {
        const QString reportId = seedSkippedReport(QStringLiteral("hb-A"));
        m_svc->enqueuePendingPostForTest(
            m_convA, reportId, QStringLiteral("body"), QStringLiteral("manual"));
        QCOMPARE(m_svc->pendingPostCountForTest(m_convA), 1);

        m_svc->invokeStreamFinalizedForTest(
            QStringLiteral("nonexistent-msg-id"), QStringLiteral("stop"), true);

        QCOMPARE(m_svc->pendingPostCountForTest(m_convA), 1);
    }

    void test_doesNotDrainOtherConvBuckets() {
        const QString reportA = seedSkippedReport(QStringLiteral("hb-A"));
        const QString reportB = seedSkippedReport(QStringLiteral("hb-B"));

        m_svc->enqueuePendingPostForTest(
            m_convA, reportA, QStringLiteral("a"), QStringLiteral("manual"));
        m_svc->enqueuePendingPostForTest(
            m_convB, reportB, QStringLiteral("b"), QStringLiteral("manual"));

        const QString fgMsgIdA = persistDummyAssistantMessage(m_convA);
        m_svc->invokeStreamFinalizedForTest(fgMsgIdA, QStringLiteral("stop"), true);

        QCOMPARE(m_svc->pendingPostCountForTest(m_convA), 0);
        QCOMPARE(m_svc->pendingPostCountForTest(m_convB), 1);
    }

    void test_drainsInFifoOrder() {
        const QString r1 = seedSkippedReport(QStringLiteral("hb-A"));
        const QString r2 = seedSkippedReport(QStringLiteral("hb-A"));
        const QString r3 = seedSkippedReport(QStringLiteral("hb-A"));

        m_svc->enqueuePendingPostForTest(
            m_convA, r1, QStringLiteral("first"), QStringLiteral("manual"));
        m_svc->enqueuePendingPostForTest(
            m_convA, r2, QStringLiteral("second"), QStringLiteral("manual"));
        m_svc->enqueuePendingPostForTest(
            m_convA, r3, QStringLiteral("third"), QStringLiteral("manual"));
        QCOMPARE(m_svc->pendingPostCountForTest(m_convA), 3);

        const QString fgMsgIdA = persistDummyAssistantMessage(m_convA);
        m_svc->invokeStreamFinalizedForTest(fgMsgIdA, QStringLiteral("stop"), true);

        QCOMPARE(m_svc->pendingPostCountForTest(m_convA), 0);

        const auto msgs = m_msgs->getMessages(m_convA);
        QStringList seenHbBodies;
        for (const auto& m : msgs) {
            if (m.role != QStringLiteral("assistant"))
                continue;
            if (m.id == fgMsgIdA)
                continue;
            seenHbBodies.append(m.content);
        }
        QCOMPARE(seenHbBodies.size(), 3);
        QCOMPARE(seenHbBodies[0], QStringLiteral("first"));
        QCOMPARE(seenHbBodies[1], QStringLiteral("second"));
        QCOMPARE(seenHbBodies[2], QStringLiteral("third"));
    }

  private:
    void seedConfig(const QString& configId, const QString& convId) {
        HeartbeatConfig cfg;
        cfg.id = configId;
        cfg.agentId = QStringLiteral("agent-sc");
        cfg.scopeType = HeartbeatScopeType::Conversation1to1;
        cfg.scopeId = convId;
        cfg.enabled = true;
        cfg.schedule = QString();
        cfg.goal = QStringLiteral("scollision goal");
        cfg.maxRunsPerDay = 100;
        QVERIFY(!m_configSvc->upsertConfig(cfg).isEmpty());
    }

    QString seedSkippedReport(const QString& configId) {
        const QString reportId =
            QStringLiteral("sc-rep-%1")
                .arg(QDateTime::currentMSecsSinceEpoch())
                .append(QStringLiteral("-"))
                .append(QString::number(QRandomGenerator::global()->generate()));
        const QString isoNow = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
        QSqlQuery q(DbManager::instance().db());
        q.prepare(QStringLiteral("INSERT INTO heartbeat_reports ("
                                 "  id, config_id, started_at, completed_at, outcome,"
                                 "  title, body, summary, parent_review_status, surface_status,"
                                 "  surfaced_message_id, error)"
                                 " VALUES (?, ?, ?, ?, 'success', 't', 'b', 's',"
                                 "  'not_reviewable_yet', 'skipped_by_gate', '', '')"));
        q.addBindValue(reportId);
        q.addBindValue(configId);
        q.addBindValue(isoNow);
        q.addBindValue(isoNow);
        if (!q.exec())
            qWarning() << "seedSkippedReport:" << q.lastError().text();
        return reportId;
    }

    QString persistDummyAssistantMessage(const QString& convId) {
        Message m;
        m.id = QStringLiteral("fg-%1-%2")
                   .arg(QDateTime::currentMSecsSinceEpoch())
                   .arg(QRandomGenerator::global()->generate());
        m.conversationId = convId;
        m.role = QStringLiteral("assistant");
        m.content = QStringLiteral("foreground stream finalized body");
        m.createdAt = QDateTime::currentDateTimeUtc();
        m.finishReason = QStringLiteral("stop");
        return m_msgs->addMessage(m);
    }

    QTemporaryDir m_tempDir;
    QString m_dbPath;
    QString m_convA;
    QString m_convB;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<ToolService> m_toolSvc;
    std::unique_ptr<SkillService> m_skillSvc;
    std::unique_ptr<AgentRegistry> m_agents;
    std::unique_ptr<ConversationService> m_convs;
    std::unique_ptr<MessageService> m_msgs;
    std::unique_ptr<Chat::RequestBuilder> m_reqBuilder;
    std::unique_ptr<HeartbeatConfigService> m_configSvc;
    std::unique_ptr<HeartbeatSubagentService> m_svc;
};

QTEST_MAIN(TestHeartbeatStreamCollision)
#include "test-heartbeat-stream-collision.moc"
