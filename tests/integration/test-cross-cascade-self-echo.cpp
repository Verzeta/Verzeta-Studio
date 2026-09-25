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

class TestCrossCascadeSelfEcho : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<MessageService> m_msgSvc;
    std::unique_ptr<ExportService> m_exportSvc;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<MembershipService> m_membership;
    std::unique_ptr<AgentRegistry> m_agentRegistry;
    QString m_convId;
    QString m_coordId;
    QString m_researcherId;

    bool driveAndCheckSaturation(Chat::CascadeController& cc, const QString& content) {
        cc.setCurrentResponder(QStringLiteral("Coord"), m_coordId);

        QSignalSpy retrySpy(&cc, &Chat::CascadeController::echoDetectedRequestRetry);

        Chat::CascadeRouteInputs inputs;
        inputs.convId = m_convId;
        inputs.content = content;
        inputs.responderMsgId = QStringLiteral("msg-id-test");
        inputs.finishReason = QStringLiteral("stop");
        inputs.requestId = 1;
        cc.routeOrFinalize(inputs);

        return retrySpy.count() > 0;
    }

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

        m_convId = m_convSvc->createGroupConversation(
            QStringLiteral("CrossCascade"), QStringList{m_coordId, m_researcherId}, QString{});
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
        m_agentRegistry.reset();
        m_membership.reset();
        m_router.reset();
        m_exportSvc.reset();
        m_msgSvc.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    }

    void test_crossCascade_detection_after_cascade_end() {
        Chat::CascadeController cc(*m_convSvc, *m_router);
        cc.setMembershipService(m_membership.get());

        const QString identicalContent =
            QStringLiteral("Hello team. I am the coordinator agent. "
                           "I will help organize our work and coordinate efforts.");

        const bool firstSaturated = driveAndCheckSaturation(cc, identicalContent);
        QVERIFY2(!firstSaturated,
                 "First cascade with this content has nothing to "
                 "compare against — must NOT saturate.");

        const bool crossCascadeSaturated = driveAndCheckSaturation(cc, identicalContent);
        QVERIFY2(crossCascadeSaturated,
                 "Cross-cascade saturation MUST fire when the SAME "
                 "content appears across two separate cascades "
                 "within the same conversation. If this assertion "
                 "fails, either the m_priorCascadeFingerprints loop "
                 "in isCascadeSaturated was removed, OR the "
                 "rollover in emitCascadeCompleteAndClear was "
                 "removed — both regress the iter17 self-echo fix.");
    }

    void test_priorCascade_window_bounded_to_K_cascades() {
        Chat::CascadeController cc(*m_convSvc, *m_router);
        cc.setMembershipService(m_membership.get());

        const QString oldContent = QStringLiteral("Some very old initial content from long ago. "
                                                  "This phrase should be evicted from the window.");

        QVERIFY(!driveAndCheckSaturation(cc, oldContent));

        for (int i = 0; i < 25; ++i) {
            const QString filler = QStringLiteral("Filler cascade number %1 about topic %2 with "
                                                  "vocabulary alpha%3 beta%4 gamma%5.")
                                       .arg(i)
                                       .arg(i * 100)
                                       .arg(i)
                                       .arg(i + 1000)
                                       .arg(i + 5000);
            QVERIFY(!driveAndCheckSaturation(cc, filler));
        }

        const bool stillInWindow = driveAndCheckSaturation(cc, oldContent);
        QVERIFY2(!stillInWindow,
                 "After 25 intervening cascades, the oldest "
                 "fingerprint MUST have been evicted from the "
                 "K=2-cascades window (max 20 entries). If this "
                 "fails, kCrossCascadeFingerprintMaxEntries was "
                 "bumped too high or the trim loop in "
                 "emitCascadeCompleteAndClear was removed.");
    }

    void test_clear_disables_crossCascade_detection() {
        Chat::CascadeController cc(*m_convSvc, *m_router);
        cc.setMembershipService(m_membership.get());

        const QString content =
            QStringLiteral("Hello team. I am the coordinator agent. "
                           "I will help organize our work and coordinate efforts.");

        QVERIFY(!driveAndCheckSaturation(cc, content));

        cc.clearCrossCascadeFingerprints();

        const bool stillSaturates = driveAndCheckSaturation(cc, content);
        QVERIFY2(!stillSaturates,
                 "After clearCrossCascadeFingerprints(), a "
                 "subsequent identical-content turn must NOT trip "
                 "saturation.");
    }

    void test_impersonatedReply_retriesWithReason_thenExhausts() {
        Chat::CascadeController cc(*m_convSvc, *m_router);
        cc.setMembershipService(m_membership.get());
        cc.setCurrentResponder(QStringLiteral("Coord"), m_coordId);

        QSignalSpy retrySpy(&cc, &Chat::CascadeController::echoDetectedRequestRetry);
        QSignalSpy exhaustSpy(&cc, &Chat::CascadeController::echoMaxRetriesExhausted);

        auto drive = [&](const QString& body) {
            Chat::CascadeRouteInputs inputs;
            inputs.convId = m_convId;
            inputs.content = body;
            inputs.responderMsgId = QStringLiteral("msg-imp");
            inputs.finishReason = QStringLiteral("stop");
            inputs.requestId = 7;
            inputs.impersonatedAlias = QStringLiteral("Researcher");
            cc.routeOrFinalize(inputs);
        };

        drive(QStringLiteral("First imposter body, novel content A."));
        QCOMPARE(retrySpy.count(), 1);
        QCOMPARE(retrySpy.last().at(5).toString(), QStringLiteral("impersonation"));
        QCOMPARE(retrySpy.last().at(4).toInt(), 1);

        drive(QStringLiteral("Second imposter body, novel content B."));
        drive(QStringLiteral("Third imposter body, novel content C."));
        QCOMPARE(retrySpy.count(), 3);
        QCOMPARE(exhaustSpy.count(), 0);

        drive(QStringLiteral("Fourth imposter body, novel content D."));
        QCOMPARE(retrySpy.count(), 3);
        QCOMPARE(exhaustSpy.count(), 1);
        QCOMPARE(exhaustSpy.last().at(2).toBool(), false);
    }

    void test_deferredContinuationPredicate_matrix() {
        Chat::CascadeController cc(*m_convSvc, *m_router);
        cc.setMembershipService(m_membership.get());
        cc.setCurrentResponder(QStringLiteral("Coord"), m_coordId);

        Ragp::Classification verdict;
        verdict.selfPendingAction = true;
        verdict.pendingActionHint = QStringLiteral("draft pricing.md");
        verdict.confidence = 0.9;

        Chat::CascadeRouteInputs inputs;
        inputs.convId = m_convId;

        QVERIFY(cc.shouldRequestDeferredContinuation(verdict, inputs));

        {
            Ragp::Classification v = verdict;
            Ragp::Target t;
            t.alias = QStringLiteral("Researcher");
            v.targets.append(t);
            QVERIFY(cc.shouldRequestDeferredContinuation(v, inputs));
        }
        {
            Ragp::Classification v = verdict;
            v.routeToUser = true;
            QVERIFY(cc.shouldRequestDeferredContinuation(v, inputs));
        }
        {
            Ragp::Classification v = verdict;
            v.confidence = 0.3;
            QVERIFY(!cc.shouldRequestDeferredContinuation(v, inputs));
        }
        {
            Ragp::Classification v = verdict;
            v.selfPendingAction = false;
            QVERIFY(!cc.shouldRequestDeferredContinuation(v, inputs));
        }
        {
            Chat::CascadeRouteInputs i2 = inputs;
            i2.deferredBudgetExhausted = true;
            QVERIFY(!cc.shouldRequestDeferredContinuation(verdict, i2));
        }
        {
            cc.setCurrentResponder(QString(), QString());
            QVERIFY(!cc.shouldRequestDeferredContinuation(verdict, inputs));
        }
    }

    void test_classifyTargetAtCap_continuesRound_notSilent() {
        Chat::CascadeController cc(*m_convSvc, *m_router);
        cc.setMembershipService(m_membership.get());

        QSignalSpy continued(&cc, &Chat::CascadeController::roundContinued);
        QSignalSpy paused(&cc, &Chat::CascadeController::cascadePausedAtCap);

        auto drive = [&](const QString& alias, const QString& agentId, const QString& body) {
            cc.setCurrentResponder(alias, agentId);
            Chat::CascadeRouteInputs in;
            in.convId = m_convId;
            in.content = body;
            in.responderMsgId = QStringLiteral("m");
            in.finishReason = QStringLiteral("stop");
            cc.routeOrFinalize(in);
            QTest::qWait(5);
        };
        const QString coord = QStringLiteral("Coord");
        const QString res = QStringLiteral("Researcher");
        drive(coord, m_coordId, QStringLiteral("@Researcher start with market sizing."));
        drive(res, m_researcherId, QStringLiteral("@Coord sizing looks strong, thoughts?"));
        drive(coord, m_coordId, QStringLiteral("@Researcher now competitor pricing."));
        drive(res, m_researcherId, QStringLiteral("@Coord pricing is premium across."));
        drive(coord, m_coordId, QStringLiteral("@Researcher last: our wedge angle."));
        QCOMPARE(continued.count(), 0);

        drive(res, m_researcherId, QStringLiteral("@Coord I need your final sign-off."));

        QVERIFY2(continued.count() == 1,
                 "cap-refused classify target must trigger round "
                 "continuation (was: silent completion)");
        QCOMPARE(continued.first().at(2).toString(), coord);
        QCOMPARE(paused.count(), 0);
    }

    void test_switchConversation_clears_window_via_hook() {
        ChatController chat(*m_router, *m_convSvc, *m_msgSvc, *m_exportSvc);
        chat.setMembershipService(m_membership.get());
        auto* cascade = chat.cascadeInternal();
        chat.switchConversation(m_convId);

        const QString content =
            QStringLiteral("Hello team. I am the coordinator agent. "
                           "I will help organize our work and coordinate efforts.");

        QVERIFY(!driveAndCheckSaturation(*cascade, content));

        QVERIFY(driveAndCheckSaturation(*cascade, content));

        const QString convB = m_convSvc->createGroupConversation(
            QStringLiteral("CrossCascadeB"), QStringList{m_coordId, m_researcherId}, QString{});
        QVERIFY(!convB.isEmpty());
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
        QVERIFY(m_membership->setConversationMembers(convB, members));

        chat.switchConversation(convB);

        cascade->setCurrentResponder(QStringLiteral("Coord"), m_coordId);
        QSignalSpy retrySpy(cascade, &Chat::CascadeController::echoDetectedRequestRetry);

        Chat::CascadeRouteInputs inputs;
        inputs.convId = convB;
        inputs.content = content;
        inputs.responderMsgId = QStringLiteral("msg-id-test-B");
        inputs.finishReason = QStringLiteral("stop");
        inputs.requestId = 2;
        cascade->routeOrFinalize(inputs);

        QVERIFY2(retrySpy.count() == 0,
                 "After switchConversation, the cross-cascade "
                 "fingerprint window MUST be cleared. An identical-"
                 "content turn in the new conv must NOT trip "
                 "saturation from the prior conv's fingerprints.");
    }

    void test_stopGeneration_clears_window_via_hook() {
        ChatController chat(*m_router, *m_convSvc, *m_msgSvc, *m_exportSvc);
        chat.setMembershipService(m_membership.get());
        auto* cascade = chat.cascadeInternal();
        chat.switchConversation(m_convId);

        const QString content =
            QStringLiteral("Hello team. I am the coordinator agent. "
                           "I will help organize our work and coordinate efforts.");

        QVERIFY(!driveAndCheckSaturation(*cascade, content));

        QVERIFY(driveAndCheckSaturation(*cascade, content));

        chat.stopGeneration();

        cascade->setCurrentResponder(QStringLiteral("Coord"), m_coordId);
        QSignalSpy retrySpy(cascade, &Chat::CascadeController::echoDetectedRequestRetry);
        Chat::CascadeRouteInputs inputs;
        inputs.convId = m_convId;
        inputs.content = content;
        inputs.responderMsgId = QStringLiteral("msg-after-stop");
        inputs.finishReason = QStringLiteral("stop");
        inputs.requestId = 3;
        cascade->routeOrFinalize(inputs);

        QVERIFY2(retrySpy.count() == 0,
                 "After stopGeneration, the cross-cascade "
                 "fingerprint window MUST be cleared.");
    }
};

QTEST_MAIN(TestCrossCascadeSelfEcho)
#include "test-cross-cascade-self-echo.moc"
