// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/api/llm-interface.h"
#include "../../backend/models/agent.h"
#include "../../backend/models/db-manager.h"
#include "../../backend/models/heartbeat-config.h"
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
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>

class TestHeartbeatManualOverride : public QObject {
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
        a.id = QStringLiteral("agent-mo");
        a.name = QStringLiteral("ManualOverrideAgent");
        a.systemPrompt = QStringLiteral("you are a tester");
        a.createdAt = QDateTime::currentDateTimeUtc();
        QVERIFY(!m_agents->createAgent(a).isEmpty());
        m_convId = m_convs->createConversation(QStringLiteral("MO Conv"));
        QVERIFY(!m_convId.isEmpty());

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
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QFile::remove(m_dbPath);
    }

    void test_manualPost_persistsManualSurfaceMessage() {
        const QString configId = createConfig();
        const QString reportId = seedSkippedReport(configId,
                                                   QStringLiteral("orig title"),
                                                   QStringLiteral("orig body"),
                                                   QStringLiteral("original summary text."));

        QSignalSpy decisionSpy(m_svc.get(), &HeartbeatSubagentService::surfaceDecision);
        const QString edited = QStringLiteral("Heads up team — found something new!");
        QVERIFY(m_svc->manualPost(reportId, edited));
        QCOMPARE(decisionSpy.count(), 1);
        QCOMPARE(decisionSpy[0][1].toString(), QStringLiteral("posted_manual"));
        const QString surfacedMsgId = decisionSpy[0][2].toString();
        QVERIFY(!surfacedMsgId.isEmpty());

        QSqlQuery rq(DbManager::instance().db());
        rq.prepare(
            QStringLiteral("SELECT parent_review_status, surface_status, surfaced_message_id "
                           "FROM heartbeat_reports WHERE id = ?"));
        rq.addBindValue(reportId);
        QVERIFY(rq.exec());
        QVERIFY(rq.next());
        QCOMPARE(rq.value(0).toString(), QStringLiteral("reviewed"));
        QCOMPARE(rq.value(1).toString(), QStringLiteral("posted_manual"));
        QCOMPARE(rq.value(2).toString(), surfacedMsgId);

        QSqlQuery mq(DbManager::instance().db());
        mq.prepare(
            QStringLiteral("SELECT role, content, agent_id, metadata FROM messages WHERE id = ?"));
        mq.addBindValue(surfacedMsgId);
        QVERIFY(mq.exec());
        QVERIFY(mq.next());
        QCOMPARE(mq.value(0).toString(), QStringLiteral("assistant"));
        QCOMPARE(mq.value(1).toString(), edited);
        QCOMPARE(mq.value(2).toString(), QStringLiteral("agent-mo"));
        const QString meta = mq.value(3).toString();
        QVERIFY(meta.contains(QStringLiteral("heartbeat_surface_manual")));
        QVERIFY(meta.contains(reportId));
    }

    void test_manualDismiss_recordsDismissed_noMessageInserted() {
        const QString configId = createConfig();
        const QString reportId = seedSkippedReport(configId, "t", "b", "s");

        QSignalSpy decisionSpy(m_svc.get(), &HeartbeatSubagentService::surfaceDecision);
        QVERIFY(m_svc->manualDismiss(reportId));
        QCOMPARE(decisionSpy.count(), 1);
        QCOMPARE(decisionSpy[0][1].toString(), QStringLiteral("dismissed_by_user"));
        QVERIFY(decisionSpy[0][2].toString().isEmpty());

        QSqlQuery rq(DbManager::instance().db());
        rq.prepare(
            QStringLiteral("SELECT surface_status, surfaced_message_id FROM heartbeat_reports "
                           "WHERE id = ?"));
        rq.addBindValue(reportId);
        QVERIFY(rq.exec());
        QVERIFY(rq.next());
        QCOMPARE(rq.value(0).toString(), QStringLiteral("dismissed_by_user"));
        QVERIFY(rq.value(1).toString().isEmpty());

        const auto convMsgs = m_msgs->getMessages(m_convId);
        for (const auto& m : convMsgs) {
            QVERIFY(m.role != QStringLiteral("assistant"));
        }
    }

    void test_manualPost_idempotentOnAlreadyPosted() {
        const QString configId = createConfig();
        const QString reportId = seedPostedReport(configId, m_convId);

        QSignalSpy decisionSpy(m_svc.get(), &HeartbeatSubagentService::surfaceDecision);
        QVERIFY(m_svc->manualPost(reportId, QStringLiteral("ignored")));
        QCOMPARE(decisionSpy.count(), 0);

        int assistantCount = 0;
        for (const auto& m : m_msgs->getMessages(m_convId)) {
            if (m.role == QStringLiteral("assistant"))
                ++assistantCount;
        }
        QCOMPARE(assistantCount, 1);
    }

    void test_manualDismiss_refusedOnPosted() {
        const QString configId = createConfig();
        const QString reportId = seedPostedReport(configId, m_convId);

        QVERIFY(!m_svc->manualDismiss(reportId));

        QSqlQuery rq(DbManager::instance().db());
        rq.prepare(QStringLiteral("SELECT surface_status FROM heartbeat_reports WHERE id = ?"));
        rq.addBindValue(reportId);
        QVERIFY(rq.exec());
        QVERIFY(rq.next());
        QCOMPARE(rq.value(0).toString(), QStringLiteral("posted_auto"));
    }

    void test_manualPost_rejectsEmptyBody() {
        const QString configId = createConfig();
        const QString reportId = seedSkippedReport(configId, "t", "b", "s");

        QVERIFY(!m_svc->manualPost(reportId, QString()));
        QVERIFY(!m_svc->manualPost(reportId, QStringLiteral("   \t\n   ")));

        QSqlQuery rq(DbManager::instance().db());
        rq.prepare(QStringLiteral("SELECT surface_status FROM heartbeat_reports WHERE id = ?"));
        rq.addBindValue(reportId);
        QVERIFY(rq.exec());
        QVERIFY(rq.next());
        QCOMPARE(rq.value(0).toString(), QStringLiteral("skipped_by_gate"));
    }

    void test_unknownReportId_failsCleanly() {
        QSignalSpy decisionSpy(m_svc.get(), &HeartbeatSubagentService::surfaceDecision);
        QVERIFY(!m_svc->manualPost(QStringLiteral("does-not-exist"), QStringLiteral("body")));
        QVERIFY(!m_svc->manualDismiss(QStringLiteral("does-not-exist")));
        QCOMPARE(decisionSpy.count(), 0);
    }

  private:
    QString createConfig() {
        HeartbeatConfig cfg;
        cfg.id = QStringLiteral("hb-mo-1");
        cfg.agentId = QStringLiteral("agent-mo");
        cfg.scopeType = HeartbeatScopeType::Conversation1to1;
        cfg.scopeId = m_convId;
        cfg.enabled = true;
        cfg.schedule = QString();
        cfg.goal = QStringLiteral("manual override goal");
        cfg.maxRunsPerDay = 100;
        const QString id = m_configSvc->upsertConfig(cfg);
        if (id.isEmpty()) {
            qWarning() << "createConfig: upsertConfig failed";
        }
        return id;
    }

    QString seedSkippedReport(const QString& configId,
                              const QString& title,
                              const QString& body,
                              const QString& summary) {
        const QString reportId = QStringLiteral("rep-%1").arg(QDateTime::currentMSecsSinceEpoch());
        const QString isoNow = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);
        QSqlQuery q(DbManager::instance().db());
        q.prepare(QStringLiteral("INSERT INTO heartbeat_reports ("
                                 "  id, config_id, started_at, completed_at, outcome,"
                                 "  title, body, summary, parent_review_status, surface_status,"
                                 "  surfaced_message_id, error)"
                                 " VALUES (?, ?, ?, ?, 'success', ?, ?, ?, "
                                 "  'not_reviewable_yet', 'skipped_by_gate', '', '')"));
        q.addBindValue(reportId);
        q.addBindValue(configId);
        q.addBindValue(isoNow);
        q.addBindValue(isoNow);
        q.addBindValue(title);
        q.addBindValue(body);
        q.addBindValue(summary);
        if (!q.exec()) {
            qWarning() << "seedSkippedReport failed:" << q.lastError().text();
        }
        return reportId;
    }

    QString seedPostedReport(const QString& configId, const QString& convId) {
        const QString reportId =
            QStringLiteral("rep-posted-%1").arg(QDateTime::currentMSecsSinceEpoch());
        const QString msgId =
            QStringLiteral("msg-posted-%1").arg(QDateTime::currentMSecsSinceEpoch());
        const QString isoNow = QDateTime::currentDateTime().toString(Qt::ISODateWithMs);

        QSqlQuery insMsg(DbManager::instance().db());
        insMsg.prepare(
            QStringLiteral("INSERT INTO messages (id, conversation_id, role, content,"
                           "  created_at, token_count, finish_reason, metadata, agent_id,"
                           "  member_alias, turn_id) "
                           "VALUES (?, ?, ?, ?, ?, 0, ?, ?, ?, ?, ?)"));
        insMsg.addBindValue(msgId);
        insMsg.addBindValue(convId);
        insMsg.addBindValue(QStringLiteral("assistant"));
        insMsg.addBindValue(QStringLiteral("seed body"));
        insMsg.addBindValue(QDateTime::currentMSecsSinceEpoch());
        insMsg.addBindValue(QStringLiteral("stop"));
        insMsg.addBindValue(QStringLiteral("{\"produced_by\":\"heartbeat_surface_auto\"}"));
        insMsg.addBindValue(QStringLiteral("agent-mo"));
        insMsg.addBindValue(QString());
        insMsg.addBindValue(msgId);
        if (!insMsg.exec()) {
            qWarning() << "seedPostedReport msg insert failed:" << insMsg.lastError().text();
        }

        QSqlQuery insRep(DbManager::instance().db());
        insRep.prepare(
            QStringLiteral("INSERT INTO heartbeat_reports ("
                           "  id, config_id, started_at, completed_at, outcome,"
                           "  title, body, summary, parent_review_status, surface_status,"
                           "  surfaced_message_id, error)"
                           " VALUES (?, ?, ?, ?, 'success', '', 'b', 's',"
                           "  'reviewed', 'posted_auto', ?, '')"));
        insRep.addBindValue(reportId);
        insRep.addBindValue(configId);
        insRep.addBindValue(isoNow);
        insRep.addBindValue(isoNow);
        insRep.addBindValue(msgId);
        if (!insRep.exec()) {
            qWarning() << "seedPostedReport rep insert failed:" << insRep.lastError().text();
        }
        return reportId;
    }

    QTemporaryDir m_tempDir;
    QString m_dbPath;
    QString m_convId;
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

QTEST_MAIN(TestHeartbeatManualOverride)
#include "test-heartbeat-manual-override.moc"
