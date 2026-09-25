// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/models/db-manager.h"
#include "../../backend/models/member.h"
#include "../../backend/services/agent-registry.h"
#include "../../backend/services/chat-controller.h"
#include "../../backend/services/chat/cascade-controller.h"
#include "../../backend/services/conversation-service.h"
#include "../../backend/services/export-service.h"
#include "../../backend/services/membership-service.h"
#include "../../backend/services/message-service.h"
#include "../../backend/services/model-router.h"

#include <QTemporaryDir>
#include <QtTest>

#include <memory>
#include <QSignalSpy>
#include <QSqlDatabase>

class TestCrossSessionCascadeIsolation : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<MessageService> m_msgSvc;
    std::unique_ptr<ExportService> m_exportSvc;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<MembershipService> m_membership;
    std::unique_ptr<AgentRegistry> m_agentRegistry;
    std::unique_ptr<ChatController> m_localCC;
    std::unique_ptr<ChatController> m_wireCC;
    QString m_convId;
    QString m_coordId;
    QString m_researcherId;

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
        m_exportSvc = std::make_unique<ExportService>(*m_convSvc, *m_msgSvc);
        m_router = std::make_unique<ModelRouter>();
        m_membership = std::make_unique<MembershipService>(DbManager::instance());
        m_agentRegistry = std::make_unique<AgentRegistry>(DbManager::instance());
        m_agentRegistry->initialize();

        m_coordId = m_agentRegistry->getOrCreateBuiltinByName(QStringLiteral("Project Manager"));
        m_researcherId = m_agentRegistry->getOrCreateBuiltinByName(QStringLiteral("Researcher"));
        QVERIFY(!m_coordId.isEmpty());
        QVERIFY(!m_researcherId.isEmpty());

        m_localCC =
            std::make_unique<ChatController>(*m_router, *m_convSvc, *m_msgSvc, *m_exportSvc);
        m_wireCC = std::make_unique<ChatController>(*m_router, *m_convSvc, *m_msgSvc, *m_exportSvc);
        m_localCC->setMembershipService(m_membership.get());
        m_wireCC->setMembershipService(m_membership.get());

        const QString folderId = m_convSvc->createFolder(QStringLiteral("Isolation"));
        m_convId = m_convSvc->createGroupConversation(
            QStringLiteral("Shared conv"), QStringList{m_coordId, m_researcherId}, folderId);
        QVERIFY(!m_convId.isEmpty());

        QList<Member> members;
        for (const auto& pair :
             QList<QPair<QString, QString>>{{m_coordId, QStringLiteral("Coord")},
                                            {m_researcherId, QStringLiteral("Researcher")}}) {
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
        m_wireCC.reset();
        m_localCC.reset();
        m_agentRegistry.reset();
        m_membership.reset();
        m_router.reset();
        m_exportSvc.reset();
        m_msgSvc.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    }

    void test_each_CC_owns_distinct_cascadeController() {
        Chat::CascadeController* local = m_localCC->cascadeInternal();
        Chat::CascadeController* wire = m_wireCC->cascadeInternal();
        QVERIFY(local != nullptr);
        QVERIFY(wire != nullptr);
        QVERIFY2(local != wire,
                 "Local CC and wire CC MUST own distinct "
                 "CascadeController instances. Sharing one breaks "
                 "per-session isolation — a wire "
                 "agent's request_turn would perturb the local "
                 "QML's cascade queue.");
    }

    void test_enqueueTarget_on_wire_does_not_touch_local() {
        auto* localCascade = m_localCC->cascadeInternal();
        auto* wireCascade = m_wireCC->cascadeInternal();

        localCascade->setCurrentResponder(QStringLiteral("Coord"), m_coordId);
        wireCascade->setCurrentResponder(QStringLiteral("Coord"), m_coordId);

        QVERIFY(wireCascade->enqueueTarget(QStringLiteral("Researcher")));
        QCOMPARE(wireCascade->queueSize(), 1);

        QCOMPARE(localCascade->queueSize(), 0);
    }

    void test_yield_signal_isolated_per_instance() {
        auto* localCascade = m_localCC->cascadeInternal();
        auto* wireCascade = m_wireCC->cascadeInternal();

        localCascade->setCurrentResponder(QStringLiteral("Coord"), m_coordId);
        wireCascade->setCurrentResponder(QStringLiteral("Coord"), m_coordId);

        wireCascade->enqueueTarget(QStringLiteral("Researcher"));

        QSignalSpy localSpy(localCascade, &Chat::CascadeController::memberTurnStarted);
        QSignalSpy wireSpy(wireCascade, &Chat::CascadeController::memberTurnStarted);

        const bool yielded = wireCascade->yieldToQueuedAfterToolBatch(m_convId);
        QVERIFY(yielded);
        QCOMPARE(wireSpy.count(), 1);
        QCOMPARE(localSpy.count(), 0);
    }

    void test_forceCascadeComplete_isolated_per_instance() {
        auto* localCascade = m_localCC->cascadeInternal();
        auto* wireCascade = m_wireCC->cascadeInternal();

        QSignalSpy localSpy(localCascade, &Chat::CascadeController::cascadeComplete);
        QSignalSpy wireSpy(wireCascade, &Chat::CascadeController::cascadeComplete);

        Chat::CascadeRouteInputs inputs;
        inputs.convId = m_convId;
        inputs.finishReason = QStringLiteral("tool_iteration_cap");
        wireCascade->forceCascadeComplete(inputs);

        QCOMPARE(wireSpy.count(), 1);
        QCOMPARE(localSpy.count(), 0);
    }

    void test_currentResponder_isolated_per_instance() {
        auto* localCascade = m_localCC->cascadeInternal();
        auto* wireCascade = m_wireCC->cascadeInternal();

        localCascade->setCurrentResponder(QStringLiteral("Coord"), m_coordId);
        wireCascade->setCurrentResponder(QStringLiteral("Researcher"), m_researcherId);

        QCOMPARE(localCascade->currentResponderAlias(), QStringLiteral("Coord"));
        QCOMPARE(wireCascade->currentResponderAlias(), QStringLiteral("Researcher"));
    }
};

QTEST_MAIN(TestCrossSessionCascadeIsolation)
#include "test-cross-session-cascade-isolation.moc"
