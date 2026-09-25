// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/api/llm-interface.h"
#include "../../backend/models/db-manager.h"
#include "../../backend/models/llm-config.h"
#include "../../backend/models/message.h"
#include "../../backend/services/chat-controller.h"
#include "../../backend/services/chat/provider-scheduler.h"
#include "../../backend/services/conversation-service.h"
#include "../../backend/services/export-service.h"
#include "../../backend/services/message-service.h"
#include "../../backend/services/model-router.h"
#include "helpers/scripted-mock-provider.h"

#include <QTemporaryDir>
#include <QtTest>

#include <memory>
#include <QDateTime>
#include <QElapsedTimer>
#include <QSqlDatabase>

class TestConcurrentMultichat : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    QString m_dbPath;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<MessageService> m_msgSvc;
    std::unique_ptr<ExportService> m_exportSvc;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<Chat::ProviderScheduler> m_sched;
    std::unique_ptr<ChatController> m_ccA;
    std::unique_ptr<ChatController> m_ccB;
    ScriptedMockProvider* m_providerX = nullptr;
    ScriptedMockProvider* m_providerY = nullptr;

    static ScriptedMockProvider::ScriptStep textStep(const QString& t) {
        ScriptedMockProvider::ScriptStep s;
        s.chunks = {t};
        s.finishReason = QStringLiteral("stop");
        return s;
    }

    void pump(int ms = 40) { QTest::qWait(ms); }

    void pinConvToProvider(const QString& convId, const QString& providerId, const QString& model) {
        LlmConfig cfg;
        cfg.providerId = providerId;
        cfg.modelName = model;
        cfg.stream = true;
        m_convSvc->updateLlmConfig(convId, cfg);
    }

    int assistantRowCount(const QString& convId) const {
        int n = 0;
        for (const Message& m : m_msgSvc->getMessages(convId)) {
            if (m.role == QStringLiteral("assistant"))
                ++n;
        }
        return n;
    }

  private slots:
    void init() {
        m_dbPath = m_tempDir.path() +
                   QStringLiteral("/concurrent_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(m_dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_convSvc = std::make_unique<ConversationService>(DbManager::instance());
        m_msgSvc = std::make_unique<MessageService>(DbManager::instance());
        m_exportSvc = std::make_unique<ExportService>(*m_convSvc, *m_msgSvc);
        m_router = std::make_unique<ModelRouter>();
        m_sched = std::make_unique<Chat::ProviderScheduler>();

        auto px = std::make_unique<ScriptedMockProvider>(QStringLiteral("provX"),
                                                         QStringList{QStringLiteral("model-x")});
        m_providerX = px.get();
        m_router->registerProvider(std::move(px));
        auto py = std::make_unique<ScriptedMockProvider>(QStringLiteral("provY"),
                                                         QStringList{QStringLiteral("model-y")});
        m_providerY = py.get();
        m_router->registerProvider(std::move(py));
        m_router->setActiveProvider(QStringLiteral("provX"), QStringLiteral("model-x"));

        m_ccA = std::make_unique<ChatController>(*m_router, *m_convSvc, *m_msgSvc, *m_exportSvc);
        m_ccB = std::make_unique<ChatController>(*m_router, *m_convSvc, *m_msgSvc, *m_exportSvc);
        m_ccA->setProviderScheduler(m_sched.get());
        m_ccB->setProviderScheduler(m_sched.get());
    }

    void cleanup() {
        m_ccA.reset();
        m_ccB.reset();
        m_sched.reset();
        m_router.reset();
        m_exportSvc.reset();
        m_msgSvc.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QFile::remove(m_dbPath);
    }

    void test_differentProviders_bothInFlightAtOnce() {
        const QString a = m_convSvc->createConversation(QStringLiteral("A"));
        const QString b = m_convSvc->createConversation(QStringLiteral("B"));
        pinConvToProvider(a, QStringLiteral("provX"), QStringLiteral("model-x"));
        pinConvToProvider(b, QStringLiteral("provY"), QStringLiteral("model-y"));

        m_providerX->setHoldInFlight(true);
        m_providerY->setHoldInFlight(true);
        m_providerX->setScript({textStep(QStringLiteral("x-reply"))});
        m_providerY->setScript({textStep(QStringLiteral("y-reply"))});

        m_ccA->switchConversation(a);
        m_ccB->switchConversation(b);
        m_ccA->sendMessage(QStringLiteral("hi A"), {});
        m_ccB->sendMessage(QStringLiteral("hi B"), {});
        pump();

        QVERIFY2(m_providerX->hasHeldRequest(), "provider X never received its in-flight request");
        QVERIFY2(m_providerY->hasHeldRequest(), "provider Y never received its in-flight request");
        QCOMPARE(m_sched->inFlightCount(QStringLiteral("provX")), 1);
        QCOMPARE(m_sched->inFlightCount(QStringLiteral("provY")), 1);
        QVERIFY(m_ccA->isConversationGenerating(a));
        QVERIFY(m_ccB->isConversationGenerating(b));

        m_providerX->releaseHeld();
        m_providerY->releaseHeld();
        pump();
        QCOMPARE(assistantRowCount(a), 1);
        QCOMPARE(assistantRowCount(b), 1);
        QCOMPARE(m_sched->inFlightCount(QStringLiteral("provX")), 0);
        QCOMPARE(m_sched->inFlightCount(QStringLiteral("provY")), 0);
        QVERIFY(!m_sched->anyInFlight());
    }

    void test_sameProvider_serializes_thenAutoDispatch() {
        const QString a = m_convSvc->createConversation(QStringLiteral("A"));
        const QString b = m_convSvc->createConversation(QStringLiteral("B"));
        pinConvToProvider(a, QStringLiteral("provX"), QStringLiteral("model-x"));
        pinConvToProvider(b, QStringLiteral("provX"), QStringLiteral("model-x"));

        m_providerX->setHoldInFlight(true);
        m_providerX->setScript(
            {textStep(QStringLiteral("first")), textStep(QStringLiteral("second"))});

        m_ccA->switchConversation(a);
        m_ccB->switchConversation(b);
        m_ccA->sendMessage(QStringLiteral("hi A"), {});
        m_ccB->sendMessage(QStringLiteral("hi B"), {});
        pump();

        QVERIFY2(m_providerX->hasHeldRequest(), "first request never reached the provider");
        QCOMPARE(m_providerX->stepsConsumed(), 1);
        QCOMPARE(m_sched->inFlightCount(QStringLiteral("provX")), 1);
        QCOMPARE(m_sched->queuedCount(QStringLiteral("provX")), 1);

        m_providerX->releaseHeld();
        pump();
        QCOMPARE(assistantRowCount(a), 1);
        QCOMPARE(m_providerX->stepsConsumed(), 2);
        QVERIFY2(m_providerX->hasHeldRequest(),
                 "second request did not auto-dispatch after the first freed "
                 "the provider slot");
        QCOMPARE(m_sched->inFlightCount(QStringLiteral("provX")), 1);
        QCOMPARE(m_sched->queuedCount(QStringLiteral("provX")), 0);

        m_providerX->releaseHeld();
        pump();
        QCOMPARE(assistantRowCount(b), 1);
        QCOMPARE(m_sched->inFlightCount(QStringLiteral("provX")), 0);
        QVERIFY(!m_sched->anyInFlight());
    }

    void test_queuedRun_abortedBeforeDispatch_isCancelled() {
        const QString a = m_convSvc->createConversation(QStringLiteral("A"));
        const QString b = m_convSvc->createConversation(QStringLiteral("B"));
        pinConvToProvider(a, QStringLiteral("provX"), QStringLiteral("model-x"));
        pinConvToProvider(b, QStringLiteral("provX"), QStringLiteral("model-x"));

        m_providerX->setHoldInFlight(true);
        m_providerX->setScript({textStep(QStringLiteral("only-first"))});

        m_ccA->switchConversation(a);
        m_ccB->switchConversation(b);
        m_ccA->sendMessage(QStringLiteral("hi A"), {});
        m_ccB->sendMessage(QStringLiteral("hi B"), {});
        pump();
        QCOMPARE(m_sched->queuedCount(QStringLiteral("provX")), 1);

        m_ccB->stopGeneration();
        QCOMPARE(m_sched->queuedCount(QStringLiteral("provX")), 0);

        m_providerX->releaseHeld();
        pump();
        QCOMPARE(assistantRowCount(a), 1);
        QCOMPARE(assistantRowCount(b), 0);
        QCOMPARE(m_sched->inFlightCount(QStringLiteral("provX")), 0);
        QVERIFY(!m_sched->anyInFlight());
    }

    void test_runDestroyedWhileQueued_doesNotCrash() {
        const QString a = m_convSvc->createConversation(QStringLiteral("A"));
        const QString b = m_convSvc->createConversation(QStringLiteral("B"));
        pinConvToProvider(a, QStringLiteral("provX"), QStringLiteral("model-x"));
        pinConvToProvider(b, QStringLiteral("provX"), QStringLiteral("model-x"));

        m_providerX->setHoldInFlight(true);
        m_providerX->setScript({textStep(QStringLiteral("a-only"))});

        m_ccA->switchConversation(a);
        m_ccB->switchConversation(b);
        m_ccA->sendMessage(QStringLiteral("hi A"), {});
        m_ccB->sendMessage(QStringLiteral("hi B"), {});
        pump();
        QCOMPARE(m_sched->queuedCount(QStringLiteral("provX")), 1);

        m_ccB.reset();
        QCOMPARE(m_sched->queuedCount(QStringLiteral("provX")), 0);

        m_providerX->releaseHeld();
        pump();
        QCOMPARE(assistantRowCount(a), 1);
        QCOMPARE(m_sched->inFlightCount(QStringLiteral("provX")), 0);
        QVERIFY(!m_sched->anyInFlight());
    }
};

QTEST_MAIN(TestConcurrentMultichat)
#include "test-concurrent-multichat.moc"
