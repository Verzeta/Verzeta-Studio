// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/models/db-manager.h"
#include "../../backend/models/member.h"
#include "../../backend/services/agent-registry.h"
#include "../../backend/services/chat/cascade-controller.h"
#include "../../backend/services/conversation-service.h"
#include "../../backend/services/membership-service.h"
#include "../../backend/services/message-service.h"
#include "../../backend/services/model-router.h"
#include "../../backend/tools/cascade/request-turn-tool.h"

#include <QTemporaryDir>
#include <QtTest>

#include <memory>
#include <QSignalSpy>
#include <QSqlDatabase>

class TestCascadeMultiAgentScenario : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<MessageService> m_msgSvc;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<MembershipService> m_membership;
    std::unique_ptr<AgentRegistry> m_agentRegistry;
    QString m_folderId;
    QString m_convId;
    QString m_coordId;
    QString m_researcherId;
    QString m_designerId;

  private slots:
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
        m_router = std::make_unique<ModelRouter>();
        m_membership = std::make_unique<MembershipService>(DbManager::instance());
        m_agentRegistry = std::make_unique<AgentRegistry>(DbManager::instance());
        m_agentRegistry->initialize();

        m_coordId = m_agentRegistry->getOrCreateBuiltinByName(QStringLiteral("Project Manager"));
        m_researcherId = m_agentRegistry->getOrCreateBuiltinByName(QStringLiteral("Researcher"));
        m_designerId = m_agentRegistry->getOrCreateBuiltinByName(QStringLiteral("Designer"));
        QVERIFY(!m_coordId.isEmpty());
        QVERIFY(!m_researcherId.isEmpty());
        QVERIFY(!m_designerId.isEmpty());

        m_folderId = m_convSvc->createFolder(QStringLiteral("CascadeTest"));
        QVERIFY(!m_folderId.isEmpty());

        m_convId =
            m_convSvc->createGroupConversation(QStringLiteral("Cascade scenario"),
                                               QStringList{m_coordId, m_researcherId, m_designerId},
                                               QString{});
        QVERIFY(!m_convId.isEmpty());

        QList<Member> members;
        for (const auto& pair :
             QList<QPair<QString, QString>>{{m_coordId, QStringLiteral("Coord")},
                                            {m_researcherId, QStringLiteral("Researcher")},
                                            {m_designerId, QStringLiteral("Designer")}}) {
            Member m;
            m.agentId = pair.first;
            m.alias = pair.second;
            m.isCoordinator = (pair.second == QStringLiteral("Coord"));
            m.joinedAt = QDateTime::currentDateTimeUtc();
            members.append(m);
        }
        QVERIFY(m_membership->setConversationMembers(m_convId, members));
    }

    void cleanup() {
        m_agentRegistry.reset();
        m_membership.reset();
        m_router.reset();
        m_msgSvc.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    }

    void test_requestTurn_strips_at_prefix() {
        Tools::RequestTurnTool tool;
        const QJsonValue result =
            tool.invoke(QJsonObject{{QStringLiteral("alias"), QStringLiteral("@Researcher")}});
        const QJsonObject obj = result.toObject();
        QCOMPARE(obj[QStringLiteral("requested")].toString(), QStringLiteral("Researcher"));
        QCOMPARE(obj[QStringLiteral("status")].toString(), QStringLiteral("queued"));
    }

    void test_requestTurn_passes_through_plain_alias() {
        Tools::RequestTurnTool tool;
        const QJsonValue result =
            tool.invoke(QJsonObject{{QStringLiteral("alias"), QStringLiteral("Researcher")}});
        QCOMPARE(result.toObject()[QStringLiteral("requested")].toString(),
                 QStringLiteral("Researcher"));
    }

    void test_requestTurn_trims_whitespace_and_strips_at() {
        Tools::RequestTurnTool tool;
        const QJsonValue result = tool.invoke(
            QJsonObject{{QStringLiteral("alias"), QStringLiteral("   @Researcher   ")}});
        QCOMPARE(result.toObject()[QStringLiteral("requested")].toString(),
                 QStringLiteral("Researcher"));
    }

    void test_requestTurn_rejects_empty_alias() {
        Tools::RequestTurnTool tool;
        const QJsonValue result = tool.invoke(QJsonObject{{QStringLiteral("alias"), QString{}}});
        const QJsonObject obj = result.toObject();
        QVERIFY(obj.contains(QStringLiteral("error")));
        QVERIFY(!obj.contains(QStringLiteral("requested")));
    }

    void test_requestTurn_rejects_whitespace_only() {
        Tools::RequestTurnTool tool;
        const QJsonValue result =
            tool.invoke(QJsonObject{{QStringLiteral("alias"), QStringLiteral("   ")}});
        const QJsonObject obj = result.toObject();
        QVERIFY(obj.contains(QStringLiteral("error")));
    }

    void test_yieldToQueuedAfterToolBatch_emptyQueue_returnsFalse() {
        Chat::CascadeController cc(*m_convSvc, *m_router);
        cc.setMembershipService(m_membership.get());
        cc.setCurrentResponder(QStringLiteral("Coord"), m_coordId);

        QSignalSpy spy(&cc, &Chat::CascadeController::memberTurnStarted);
        QVERIFY(spy.isValid());

        const bool yielded = cc.yieldToQueuedAfterToolBatch(m_convId);
        QVERIFY2(!yielded, "Empty queue must return false — no member to yield to.");
        QCOMPARE(spy.count(), 0);
    }

    void test_yieldToQueuedAfterToolBatch_queuedTarget_dispatches() {
        Chat::CascadeController cc(*m_convSvc, *m_router);
        cc.setMembershipService(m_membership.get());
        cc.setCurrentResponder(QStringLiteral("Coord"), m_coordId);

        const bool enqueued = cc.enqueueTarget(QStringLiteral("Researcher"));
        QVERIFY(enqueued);
        QCOMPARE(cc.queueSize(), 1);

        QSignalSpy spy(&cc, &Chat::CascadeController::memberTurnStarted);
        QVERIFY(spy.isValid());

        const bool yielded = cc.yieldToQueuedAfterToolBatch(m_convId);
        QVERIFY2(yielded,
                 "Non-empty queue must yield — this is the iter16 "
                 "cascade-starve fix (commit 1b5c69d).");
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("Researcher"));
        QCOMPARE(spy.at(0).at(1).toString(), m_researcherId);

        QCOMPARE(cc.queueSize(), 0);
    }

    void test_forceCascadeComplete_clears_state_and_emits() {
        Chat::CascadeController cc(*m_convSvc, *m_router);
        cc.setMembershipService(m_membership.get());
        cc.setCurrentResponder(QStringLiteral("Coord"), m_coordId);

        cc.enqueueTarget(QStringLiteral("Researcher"));
        cc.enqueueTarget(QStringLiteral("Designer"));
        QCOMPARE(cc.queueSize(), 2);

        QSignalSpy spy(&cc, &Chat::CascadeController::cascadeComplete);
        QVERIFY(spy.isValid());

        Chat::CascadeRouteInputs inputs;
        inputs.convId = m_convId;
        inputs.finishReason = QStringLiteral("tool_iteration_cap");
        inputs.totalTokens = 42;
        inputs.elapsedMs = 123;
        cc.forceCascadeComplete(inputs);

        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("tool_iteration_cap"));
        QCOMPARE(spy.at(0).at(2).toInt(), 42);
        QCOMPARE(spy.at(0).at(3).toLongLong(), qint64{123});

        QCOMPARE(cc.queueSize(), 0);
    }

    void test_enqueueTarget_self_is_rejected() {
        Chat::CascadeController cc(*m_convSvc, *m_router);
        cc.setMembershipService(m_membership.get());
        cc.setCurrentResponder(QStringLiteral("Coord"), m_coordId);

        const bool enqueuedSelf = cc.enqueueTarget(QStringLiteral("Coord"));
        QVERIFY2(!enqueuedSelf,
                 "Cascade must not queue an agent for its OWN turn "
                 "— that's the self-mention loop class.");
        QCOMPARE(cc.queueSize(), 0);
    }

    void test_enqueueTarget_dedup_in_queue() {
        Chat::CascadeController cc(*m_convSvc, *m_router);
        cc.setMembershipService(m_membership.get());
        cc.setCurrentResponder(QStringLiteral("Coord"), m_coordId);

        QVERIFY(cc.enqueueTarget(QStringLiteral("Researcher")));
        QVERIFY(!cc.enqueueTarget(QStringLiteral("Researcher")));
        QCOMPARE(cc.queueSize(), 1);
    }
};

QTEST_MAIN(TestCascadeMultiAgentScenario)
#include "test-cascade-multi-agent-scenario.moc"
