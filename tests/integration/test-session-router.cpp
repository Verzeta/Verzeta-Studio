// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/db-manager.h"
#include "models/llm-config.h"
#include "services/agent-settings-controller.h"
#include "services/chat-controller.h"
#include "services/conversation-service.h"
#include "services/export-service.h"
#include "services/message-service.h"
#include "services/model-router.h"
#include "services/session/session-router.h"

#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <QSignalSpy>
#include <QSqlDatabase>

class TestSessionRouter : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<MessageService> m_msgSvc;
    std::unique_ptr<ExportService> m_exportSvc;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<ChatController> m_localCC;

    using SessionRouter = Verzeta::Session::SessionRouter;

  private slots:

    void init() {
        const QString dbPath =
            m_tempDir.path() +
            QStringLiteral("/sessrouter_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_convSvc = std::make_unique<ConversationService>(DbManager::instance());
        m_msgSvc = std::make_unique<MessageService>(DbManager::instance());
        m_exportSvc = std::make_unique<ExportService>(*m_convSvc, *m_msgSvc);
        m_router = std::make_unique<ModelRouter>();

        m_localCC =
            std::make_unique<ChatController>(*m_router, *m_convSvc, *m_msgSvc, *m_exportSvc);
    }

    void cleanup() {
        m_localCC.reset();
        m_router.reset();
        m_exportSvc.reset();
        m_msgSvc.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    }


    void test_registerLocalSession_holdsNonOwningPointer() {
        SessionRouter sr(*m_convSvc, *m_msgSvc, *m_router, *m_exportSvc);
        QCOMPARE(sr.sessionCount(), 0);

        sr.registerLocalSession(m_localCC.get());

        QCOMPARE(sr.sessionCount(), 1);
        QCOMPARE(sr.localSession(), m_localCC.get());
        QCOMPARE(sr.sessionFor(QString::fromUtf8(Verzeta::Session::kLocalSessionId)),
                 m_localCC.get());
    }

    void test_destroySession_localIdDoesNotDeleteAppControllerOwned() {
        SessionRouter sr(*m_convSvc, *m_msgSvc, *m_router, *m_exportSvc);
        sr.registerLocalSession(m_localCC.get());

        sr.destroySession(QString::fromUtf8(Verzeta::Session::kLocalSessionId));

        QVERIFY(m_localCC);
        QCOMPARE(sr.localSession(), nullptr);
    }


    void test_createWireSession_constructsNewChatController() {
        SessionRouter sr(*m_convSvc, *m_msgSvc, *m_router, *m_exportSvc);
        sr.registerLocalSession(m_localCC.get());

        ChatController* wireCC = sr.createWireSession(QStringLiteral("wire-uuid-1"));
        QVERIFY(wireCC != nullptr);
        QVERIFY(wireCC != m_localCC.get());
        QCOMPARE(sr.sessionFor(QStringLiteral("wire-uuid-1")), wireCC);
        QCOMPARE(sr.sessionCount(), 2);
    }

    void test_createWireSession_emptyIdRejected() {
        SessionRouter sr(*m_convSvc, *m_msgSvc, *m_router, *m_exportSvc);
        QCOMPARE(sr.createWireSession(QString()), nullptr);
        QCOMPARE(sr.sessionCount(), 0);
    }

    void test_createWireSession_localIdRejected() {
        SessionRouter sr(*m_convSvc, *m_msgSvc, *m_router, *m_exportSvc);
        QCOMPARE(sr.createWireSession(QString::fromUtf8(Verzeta::Session::kLocalSessionId)),
                 nullptr);
        QCOMPARE(sr.sessionCount(), 0);
    }

    void test_createWireSession_duplicateIdReturnsExistingInstance() {
        SessionRouter sr(*m_convSvc, *m_msgSvc, *m_router, *m_exportSvc);

        ChatController* first = sr.createWireSession(QStringLiteral("dup-id"));
        QVERIFY(first != nullptr);

        ChatController* second = sr.createWireSession(QStringLiteral("dup-id"));
        QCOMPARE(second, first);
        QCOMPARE(sr.sessionCount(), 1);
    }

    void test_destroySession_wireIdRemovesInstance() {
        SessionRouter sr(*m_convSvc, *m_msgSvc, *m_router, *m_exportSvc);

        ChatController* wireCC = sr.createWireSession(QStringLiteral("wire-uuid-2"));
        QVERIFY(wireCC != nullptr);
        QCOMPARE(sr.sessionCount(), 1);

        sr.destroySession(QStringLiteral("wire-uuid-2"));
        QCOMPARE(sr.sessionFor(QStringLiteral("wire-uuid-2")), nullptr);
        QCOMPARE(sr.sessionCount(), 0);
    }

    void test_multipleWireSessions_coexist() {
        SessionRouter sr(*m_convSvc, *m_msgSvc, *m_router, *m_exportSvc);
        sr.registerLocalSession(m_localCC.get());

        ChatController* a = sr.createWireSession(QStringLiteral("a"));
        ChatController* b = sr.createWireSession(QStringLiteral("b"));
        ChatController* c = sr.createWireSession(QStringLiteral("c"));

        QVERIFY(a && b && c);
        QVERIFY(a != b);
        QVERIFY(b != c);
        QVERIFY(a != c);
        QCOMPARE(sr.sessionCount(), 4);
    }


    void test_setupCallback_invokedPerCreatedWireSession() {
        SessionRouter sr(*m_convSvc, *m_msgSvc, *m_router, *m_exportSvc);

        int invocations = 0;
        ChatController* lastCC = nullptr;
        sr.setChatControllerSetupFn([&](ChatController* cc) {
            ++invocations;
            lastCC = cc;
        });

        ChatController* one = sr.createWireSession(QStringLiteral("one"));
        QCOMPARE(invocations, 1);
        QCOMPARE(lastCC, one);

        ChatController* two = sr.createWireSession(QStringLiteral("two"));
        QCOMPARE(invocations, 2);
        QCOMPARE(lastCC, two);
    }

    void test_setupCallback_notCalledForLocalRegistration() {
        SessionRouter sr(*m_convSvc, *m_msgSvc, *m_router, *m_exportSvc);

        int invocations = 0;
        sr.setChatControllerSetupFn([&](ChatController*) { ++invocations; });

        sr.registerLocalSession(m_localCC.get());
        QCOMPARE(invocations, 0);
    }

    void test_missingSetupCallback_stillConstructsWireInstance() {
        SessionRouter sr(*m_convSvc, *m_msgSvc, *m_router, *m_exportSvc);
        ChatController* cc = sr.createWireSession(QStringLiteral("nosetup"));
        QVERIFY(cc != nullptr);
        QCOMPARE(sr.sessionFor(QStringLiteral("nosetup")), cc);
    }

    void test_setupCallback_clearedThenInstalled() {
        SessionRouter sr(*m_convSvc, *m_msgSvc, *m_router, *m_exportSvc);

        int invocations = 0;
        sr.setChatControllerSetupFn([&](ChatController*) { ++invocations; });

        sr.setChatControllerSetupFn({});

        ChatController* cc = sr.createWireSession(QStringLiteral("after-clear"));
        QVERIFY(cc != nullptr);
        QCOMPARE(invocations, 0);

        sr.setChatControllerSetupFn([&](ChatController*) { ++invocations; });
        ChatController* cc2 = sr.createWireSession(QStringLiteral("after-reinstall"));
        QVERIFY(cc2 != nullptr);
        QCOMPARE(invocations, 1);
    }


    void test_multipleWireSessions_independentActiveConvId() {
        SessionRouter sr(*m_convSvc, *m_msgSvc, *m_router, *m_exportSvc);
        sr.registerLocalSession(m_localCC.get());

        ChatController* a = sr.createWireSession(QStringLiteral("wire-A"));
        ChatController* b = sr.createWireSession(QStringLiteral("wire-B"));
        QVERIFY(a && b);
        QVERIFY(a != b);
        QVERIFY(a != m_localCC.get());

        QCOMPARE(a->activeConversationId(), QString());
        QCOMPARE(b->activeConversationId(), QString());
        QCOMPARE(m_localCC->activeConversationId(), QString());

        const QString convA = m_convSvc->createConversation(QStringLiteral("for wire A"));
        const QString convB = m_convSvc->createConversation(QStringLiteral("for wire B"));
        const QString convL = m_convSvc->createConversation(QStringLiteral("for local"));
        QVERIFY(!convA.isEmpty() && !convB.isEmpty() && !convL.isEmpty());

        a->switchConversation(convA);
        b->switchConversation(convB);
        m_localCC->switchConversation(convL);

        QCOMPARE(a->activeConversationId(), convA);
        QCOMPARE(b->activeConversationId(), convB);
        QCOMPARE(m_localCC->activeConversationId(), convL);
    }

    void test_multipleWireSessions_signalIsolation() {
        SessionRouter sr(*m_convSvc, *m_msgSvc, *m_router, *m_exportSvc);
        sr.registerLocalSession(m_localCC.get());

        ChatController* a = sr.createWireSession(QStringLiteral("wire-A"));
        ChatController* b = sr.createWireSession(QStringLiteral("wire-B"));

        QSignalSpy spyA(a, &ChatController::activeConversationChanged);
        QSignalSpy spyB(b, &ChatController::activeConversationChanged);
        QSignalSpy spyLocal(m_localCC.get(), &ChatController::activeConversationChanged);

        const QString convA = m_convSvc->createConversation(QStringLiteral("for wire A"));
        QVERIFY(!convA.isEmpty());

        a->switchConversation(convA);

        QCOMPARE(spyA.count(), 1);
        QCOMPARE(spyB.count(), 0);
        QCOMPARE(spyLocal.count(), 0);
    }


    void test_registerLocalAgentSettings_holdsNonOwningPointer() {
        SessionRouter sr(*m_convSvc, *m_msgSvc, *m_router, *m_exportSvc);
        QCOMPARE(sr.localAgentSettings(), nullptr);

        AgentSettingsController localAS(*m_convSvc, *m_router);
        sr.registerLocalAgentSettings(&localAS);

        QCOMPARE(sr.localAgentSettings(), &localAS);
        QCOMPARE(sr.agentSettingsFor(QString::fromUtf8(Verzeta::Session::kLocalSessionId)),
                 &localAS);
    }

    void test_createWireSession_alsoCreatesAgentSettings() {
        SessionRouter sr(*m_convSvc, *m_msgSvc, *m_router, *m_exportSvc);
        sr.registerLocalSession(m_localCC.get());

        ChatController* wireCC = sr.createWireSession(QStringLiteral("wire-as-1"));
        QVERIFY(wireCC != nullptr);

        AgentSettingsController* wireAS = sr.agentSettingsFor(QStringLiteral("wire-as-1"));
        QVERIFY(wireAS != nullptr);
        QVERIFY(wireAS != sr.localAgentSettings());
    }

    void test_createWireAgentSettings_lazyCreatesFullSession() {
        SessionRouter sr(*m_convSvc, *m_msgSvc, *m_router, *m_exportSvc);
        sr.registerLocalSession(m_localCC.get());

        AgentSettingsController* wireAS = sr.createWireAgentSettings(QStringLiteral("wire-as-2"));
        QVERIFY(wireAS != nullptr);
        QVERIFY(sr.sessionFor(QStringLiteral("wire-as-2")) != nullptr);
    }

    void test_createWireAgentSettings_duplicateIdReturnsExisting() {
        SessionRouter sr(*m_convSvc, *m_msgSvc, *m_router, *m_exportSvc);

        AgentSettingsController* first = sr.createWireAgentSettings(QStringLiteral("dup-as-id"));
        QVERIFY(first != nullptr);

        AgentSettingsController* second = sr.createWireAgentSettings(QStringLiteral("dup-as-id"));
        QCOMPARE(second, first);
    }

    void test_wireAgentSettings_followsWireCcSwitchConversation() {
        SessionRouter sr(*m_convSvc, *m_msgSvc, *m_router, *m_exportSvc);
        sr.registerLocalSession(m_localCC.get());

        ChatController* wireCC = sr.createWireSession(QStringLiteral("wire-as-3"));
        AgentSettingsController* wireAS = sr.agentSettingsFor(QStringLiteral("wire-as-3"));
        QVERIFY(wireCC && wireAS);

        const QString conv = m_convSvc->createConversation(QStringLiteral("wire-as-test"));
        QVERIFY(!conv.isEmpty());

        QCOMPARE(wireAS->activeSystemPrompt(), QString());

        wireCC->switchConversation(conv);

        m_convSvc->updateSystemPrompt(conv, QStringLiteral("hello-world"));
        QCOMPARE(wireAS->activeSystemPrompt(), QStringLiteral("hello-world"));
    }

    void test_perClientAgentSettings_isolatedFromLocal() {
        SessionRouter sr(*m_convSvc, *m_msgSvc, *m_router, *m_exportSvc);
        sr.registerLocalSession(m_localCC.get());

        AgentSettingsController localAS(*m_convSvc, *m_router);
        sr.registerLocalAgentSettings(&localAS);

        ChatController* wireCC = sr.createWireSession(QStringLiteral("wire-iso"));
        AgentSettingsController* wireAS = sr.agentSettingsFor(QStringLiteral("wire-iso"));
        QVERIFY(wireCC && wireAS);

        const QString convL = m_convSvc->createConversation(QStringLiteral("local conv"));
        const QString convW = m_convSvc->createConversation(QStringLiteral("wire conv"));
        QVERIFY(!convL.isEmpty() && !convW.isEmpty());

        m_localCC->switchConversation(convL);
        localAS.setActiveConversationId(convL);

        wireCC->switchConversation(convW);

        wireAS->setAgentPattern(QStringLiteral("react"));

        const auto wireConv = m_convSvc->getConversation(convW);
        const auto localConv = m_convSvc->getConversation(convL);
        QVERIFY(wireConv.has_value() && localConv.has_value());

        const QString wirePattern = LlmConfig::fromJson(wireConv->llmConfig).agentPattern;
        const QString localPattern = LlmConfig::fromJson(localConv->llmConfig).agentPattern;
        QCOMPARE(wirePattern, QStringLiteral("react"));
        QCOMPARE(localPattern, QString());
    }

    void test_destroySession_alsoDestroysAgentSettings() {
        SessionRouter sr(*m_convSvc, *m_msgSvc, *m_router, *m_exportSvc);

        sr.createWireSession(QStringLiteral("wire-destroy"));
        QVERIFY(sr.agentSettingsFor(QStringLiteral("wire-destroy")) != nullptr);

        sr.destroySession(QStringLiteral("wire-destroy"));
        QVERIFY(sr.agentSettingsFor(QStringLiteral("wire-destroy")) == nullptr);
        QVERIFY(sr.sessionFor(QStringLiteral("wire-destroy")) == nullptr);
    }

    void test_destroyAllWireSessions_alsoTearsDownAllAgentSettings() {
        SessionRouter sr(*m_convSvc, *m_msgSvc, *m_router, *m_exportSvc);

        sr.createWireSession(QStringLiteral("wire-tear-1"));
        sr.createWireSession(QStringLiteral("wire-tear-2"));
        QVERIFY(sr.agentSettingsFor(QStringLiteral("wire-tear-1")));
        QVERIFY(sr.agentSettingsFor(QStringLiteral("wire-tear-2")));

        sr.destroyAllWireSessions();

        QVERIFY(sr.agentSettingsFor(QStringLiteral("wire-tear-1")) == nullptr);
        QVERIFY(sr.agentSettingsFor(QStringLiteral("wire-tear-2")) == nullptr);
    }

    void test_routerDestruction_destroysAllWireSessions() {
        std::unique_ptr<SessionRouter> sr =
            std::make_unique<SessionRouter>(*m_convSvc, *m_msgSvc, *m_router, *m_exportSvc);

        ChatController* wireA = sr->createWireSession(QStringLiteral("A"));
        ChatController* wireB = sr->createWireSession(QStringLiteral("B"));
        QVERIFY(wireA && wireB);
        QCOMPARE(sr->sessionCount(), 2);

        sr.reset();
        QVERIFY(true);
    }
};

QTEST_MAIN(TestSessionRouter)
#include "test-session-router.moc"
