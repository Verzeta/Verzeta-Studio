// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/artifacts-model.h"
#include "models/db-manager.h"
#include "models/message-list-model.h"
#include "models/plans-model.h"
#include "models/tool-call-log-model.h"
#include "services/conversation-service.h"
#include "services/message-service.h"
#include "services/plan-service.h"

#include <QTemporaryDir>
#include <QTest>

#include <QDateTime>
#include <QJsonObject>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QUuid>

class TestConversationDeletionCascade : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<MessageService> m_msgSvc;
    std::unique_ptr<PlanService> m_planSvc;

    static QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

  private slots:

    void initTestCase() { QVERIFY(m_tempDir.isValid()); }

    void init() {
        const QString dbPath =
            m_tempDir.path() +
            QStringLiteral("/test_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_convSvc = std::make_unique<ConversationService>(DbManager::instance());
        m_msgSvc = std::make_unique<MessageService>(DbManager::instance());
        m_planSvc = std::make_unique<PlanService>(DbManager::instance());
    }

    void cleanup() {
        m_planSvc.reset();
        m_msgSvc.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    }


    void test_fullCascadeOnConversationDelete() {
        QObject::connect(m_convSvc.get(),
                         &ConversationService::conversationDeleted,
                         m_msgSvc.get(),
                         &MessageService::onConversationDeleted);

        const QString convId = m_convSvc->createConversation(QStringLiteral("Cascade"));
        QVERIFY(!convId.isEmpty());

        Message parentMsg;
        parentMsg.id = uuid();
        parentMsg.conversationId = convId;
        parentMsg.role = QStringLiteral("assistant");
        parentMsg.content = QString{};
        parentMsg.finishReason = QStringLiteral("tool_calls");
        parentMsg.createdAt = QDateTime::currentDateTimeUtc();
        QVERIFY(!m_msgSvc->addMessage(parentMsg).isEmpty());

        AgentPlan plan;
        plan.conversationId = convId;
        plan.goal = QStringLiteral("do the thing");
        plan.startedBy = QStringLiteral("user");
        plan.status = PlanStatus::Executing;

        QList<PlanStep> planSteps;
        PlanStep s;
        s.title = QStringLiteral("Step 1");
        s.description = QStringLiteral("first");
        s.ownerAlias = QStringLiteral("alice");
        s.acceptanceCriteria = QStringLiteral("finished");
        s.status = StepStatus::InProgress;
        planSteps.append(s);

        const QString planId = m_planSvc->createPlan(plan, planSteps);
        QVERIFY(!planId.isEmpty());
        const QList<PlanStep> persisted = m_planSvc->stepsForPlan(planId);
        QCOMPARE(persisted.size(), 1);
        const QString stepId = persisted.first().id;

        ToolCall call;
        call.id = uuid();
        call.messageId = parentMsg.id;
        call.toolName = QStringLiteral("run_shell");
        call.arguments = QJsonObject{{QStringLiteral("cmd"), QStringLiteral("ls")}};
        call.status = QStringLiteral("running");
        call.startedAt = QDateTime::currentDateTimeUtc();
        call.planStepId = stepId;
        QVERIFY(!m_msgSvc->addToolCall(call).isEmpty());

        const QString streamMsgId = uuid();
        m_msgSvc->beginStreamingMessage(convId, streamMsgId, QStringLiteral("assistant"));
        m_msgSvc->appendStreamingChunk(streamMsgId, QStringLiteral("partial"));
        QVERIFY(m_msgSvc->hasStreamingMessage(streamMsgId));

        MessageListModel msgModel(*m_msgSvc);
        msgModel.setActiveConversation(convId);
        PlansModel plansModel(*m_planSvc);
        plansModel.setActiveConversation(convId);
        QObject::connect(m_convSvc.get(),
                         &ConversationService::conversationDeleted,
                         &plansModel,
                         &PlansModel::onSourceConversationDeleted);
        ToolCallLogModel toolLogModel(*m_msgSvc);
        toolLogModel.setActiveConversation(convId);
        ArtifactsModel artifactsModel(*m_msgSvc);
        artifactsModel.setActiveConversation(convId);

        QVERIFY(msgModel.count() >= 1);
        QCOMPARE(plansModel.count(), 1);
        QCOMPARE(toolLogModel.count(), 1);

        QSignalSpy streamAbortSpy(m_msgSvc.get(), &MessageService::messageStreamingAborted);

        QVERIFY(m_convSvc->deleteConversation(convId));

        QCOMPARE(streamAbortSpy.count(), 1);
        QVERIFY(!m_msgSvc->hasStreamingMessage(streamMsgId));

        QCOMPARE(plansModel.count(), 0);

        const QList<ToolCall> remaining = m_msgSvc->toolCallsForConversation(convId);
        QCOMPARE(remaining.size(), 0);

        QVERIFY(!m_convSvc->getConversation(convId).has_value());
    }
};

QTEST_MAIN(TestConversationDeletionCascade)
#include "test-conversation-deletion-cascade.moc"
