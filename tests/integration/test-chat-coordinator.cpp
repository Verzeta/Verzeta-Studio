// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/api/llm-interface.h"
#include "../../backend/models/db-manager.h"
#include "../../backend/models/llm-config.h"
#include "../../backend/models/message-list-model.h"
#include "../../backend/models/message.h"
#include "../../backend/services/chat-controller.h"
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
#include <QSignalSpy>
#include <QSqlDatabase>

class TestChatCoordinator : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    QString m_dbPath;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<MessageService> m_msgSvc;
    std::unique_ptr<ExportService> m_exportSvc;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<ChatController> m_chat;
    ScriptedMockProvider* m_providerX = nullptr;
    ScriptedMockProvider* m_providerY = nullptr;

    static ScriptedMockProvider::ScriptStep textStep(const QString& t) {
        ScriptedMockProvider::ScriptStep s;
        s.chunks = {t};
        s.finishReason = QStringLiteral("stop");
        return s;
    }

    void waitNotGenerating(int deadlineMs = 4000) {
        QElapsedTimer t;
        t.start();
        while (m_chat->isGenerating() && t.elapsed() < deadlineMs) {
            QTest::qWait(10);
        }
    }

    void waitConvIdle(const QString& convId, int deadlineMs = 4000) {
        QElapsedTimer t;
        t.start();
        while (m_chat->isConversationGenerating(convId) && t.elapsed() < deadlineMs) {
            QTest::qWait(10);
        }
    }

    int assistantRowCount(const QString& convId) const {
        int n = 0;
        for (const Message& m : m_msgSvc->getMessages(convId)) {
            if (m.role == QStringLiteral("assistant"))
                ++n;
        }
        return n;
    }

    void pinConvToProvider(const QString& convId, const QString& providerId, const QString& model) {
        LlmConfig cfg;
        cfg.providerId = providerId;
        cfg.modelName = model;
        cfg.stream = true;
        m_convSvc->updateLlmConfig(convId, cfg);
    }

  private slots:
    void init() {
        m_dbPath = m_tempDir.path() +
                   QStringLiteral("/coord_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(m_dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_convSvc = std::make_unique<ConversationService>(DbManager::instance());
        m_msgSvc = std::make_unique<MessageService>(DbManager::instance());
        m_exportSvc = std::make_unique<ExportService>(*m_convSvc, *m_msgSvc);
        m_router = std::make_unique<ModelRouter>();

        auto px = std::make_unique<ScriptedMockProvider>(QStringLiteral("mockx"),
                                                         QStringList{QStringLiteral("model-x")});
        m_providerX = px.get();
        m_router->registerProvider(std::move(px));

        auto py = std::make_unique<ScriptedMockProvider>(QStringLiteral("mocky"),
                                                         QStringList{QStringLiteral("model-y")});
        m_providerY = py.get();
        m_router->registerProvider(std::move(py));

        m_router->setActiveProvider(QStringLiteral("mockx"), QStringLiteral("model-x"));

        m_chat = std::make_unique<ChatController>(*m_router, *m_convSvc, *m_msgSvc, *m_exportSvc);
    }

    void cleanup() {
        m_chat.reset();
        m_router.reset();
        m_exportSvc.reset();
        m_msgSvc.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QFile::remove(m_dbPath);
    }

    void test_singleConversation_lifecycleUnchanged() {
        const QString convId = m_convSvc->createConversation(QStringLiteral("Solo"));
        QVERIFY(!convId.isEmpty());
        pinConvToProvider(convId, QStringLiteral("mockx"), QStringLiteral("model-x"));
        m_chat->switchConversation(convId);
        QCOMPARE(m_chat->activeConversationId(), convId);

        m_providerX->setScript({textStep(QStringLiteral("Hello there!"))});

        QSignalSpy genSpy(m_chat.get(), &ChatController::isGeneratingChanged);
        QVERIFY(!m_chat->isGenerating());

        m_chat->sendMessage(QStringLiteral("hi"), {});
        waitNotGenerating();

        QVERIFY(!m_chat->isGenerating());
        QVERIFY2(genSpy.count() >= 2, "isGenerating did not toggle true then false");
        QCOMPARE(assistantRowCount(convId), 1);

        bool found = false;
        for (const Message& m : m_msgSvc->getMessages(convId)) {
            if (m.role == QStringLiteral("assistant") &&
                m.content == QStringLiteral("Hello there!"))
                found = true;
        }
        QVERIFY2(found, "assistant reply did not persist to its conversation");
        QCOMPARE(m_chat->runCountForTest(), 1);
    }

    void test_runLifecycle_reuseWhenIdle_secondRunWhenBackgrounded() {
        const QString a = m_convSvc->createConversation(QStringLiteral("A"));
        const QString b = m_convSvc->createConversation(QStringLiteral("B"));
        pinConvToProvider(a, QStringLiteral("mockx"), QStringLiteral("model-x"));
        pinConvToProvider(b, QStringLiteral("mocky"), QStringLiteral("model-y"));

        m_providerX->setScript({textStep(QStringLiteral("a-reply"))});
        m_providerY->setScript({textStep(QStringLiteral("b-reply"))});

        m_chat->switchConversation(a);
        m_chat->sendMessage(QStringLiteral("hello A"), {});
        waitConvIdle(a);
        QCOMPARE(m_chat->runCountForTest(), 1);

        m_chat->switchConversation(b);
        QCOMPARE(m_chat->activeConversationId(), b);
        QCOMPARE(m_chat->runCountForTest(), 1);
        m_chat->sendMessage(QStringLiteral("hello B"), {});
        waitConvIdle(b);
        QCOMPARE(m_chat->runCountForTest(), 1);
        QCOMPARE(assistantRowCount(a), 1);
        QCOMPARE(assistantRowCount(b), 1);

        m_providerY->setScript({textStep(QStringLiteral("b-second"))});
        m_chat->switchConversation(b);
        bool switched = false;
        QMetaObject::Connection c = QObject::connect(
            m_chat.get(), &ChatController::isGeneratingChanged, m_chat.get(), [&]() {
                if (!switched && m_chat->isAnyConvGenerating() &&
                    m_chat->activeConversationId() == b) {
                    switched = true;
                    m_chat->switchConversation(a);
                }
            });
        m_chat->sendMessage(QStringLiteral("hello B again"), {});
        QObject::disconnect(c);

        QVERIFY2(switched, "test never observed B's in-flight window");
        QCOMPARE(m_chat->activeConversationId(), a);
        QCOMPARE(m_chat->runCountForTest(), 2);
        waitConvIdle(b);
        QCOMPARE(assistantRowCount(b), 2);
    }

    void test_switch_repointsFacade() {
        const QString a = m_convSvc->createConversation(QStringLiteral("Alpha"));
        const QString b = m_convSvc->createConversation(QStringLiteral("Beta"));
        pinConvToProvider(a, QStringLiteral("mockx"), QStringLiteral("model-x"));
        pinConvToProvider(b, QStringLiteral("mocky"), QStringLiteral("model-y"));

        m_chat->switchConversation(a);
        QCOMPARE(m_chat->activeConversationId(), a);
        QCOMPARE(m_chat->activeConversationTitle(), QStringLiteral("Alpha"));

        QSignalSpy activeSpy(m_chat.get(), &ChatController::activeConversationChanged);

        m_chat->switchConversation(b);

        QCOMPARE(activeSpy.count(), 1);
        QCOMPARE(m_chat->activeConversationId(), b);
        QCOMPARE(m_chat->activeConversationTitle(), QStringLiteral("Beta"));
        QVERIFY(!m_chat->isGenerating());

        m_providerY->setScript({textStep(QStringLiteral("b-done"))});
        bool switchedBack = false;
        QMetaObject::Connection c = QObject::connect(
            m_chat.get(), &ChatController::isGeneratingChanged, m_chat.get(), [&]() {
                if (!switchedBack && m_chat->isAnyConvGenerating() &&
                    m_chat->activeConversationId() == b) {
                    switchedBack = true;
                    m_chat->switchConversation(a);
                }
            });
        m_chat->sendMessage(QStringLiteral("go B"), {});
        QObject::disconnect(c);

        QVERIFY2(switchedBack, "test never observed B's in-flight window");
        QCOMPARE(m_chat->activeConversationId(), a);
        QVERIFY2(!m_chat->isGenerating(),
                 "facade showed generating though only background B had a turn");
        waitConvIdle(b);
        QCOMPARE(assistantRowCount(b), 1);
    }

    void test_backgroundRun_persistsToOwnConversation() {
        const QString a = m_convSvc->createConversation(QStringLiteral("A"));
        const QString b = m_convSvc->createConversation(QStringLiteral("B"));
        pinConvToProvider(a, QStringLiteral("mockx"), QStringLiteral("model-x"));
        pinConvToProvider(b, QStringLiteral("mocky"), QStringLiteral("model-y"));

        m_providerX->setScript({textStep(QStringLiteral("from-A-on-X"))});
        m_providerY->setScript({textStep(QStringLiteral("from-B-on-Y"))});

        m_chat->switchConversation(a);
        bool switched = false;
        QMetaObject::Connection c = QObject::connect(
            m_chat.get(), &ChatController::isGeneratingChanged, m_chat.get(), [&]() {
                if (!switched && m_chat->isAnyConvGenerating() &&
                    m_chat->activeConversationId() == a) {
                    switched = true;
                    m_chat->switchConversation(b);
                }
            });
        m_chat->sendMessage(QStringLiteral("answer in A"), {});
        QObject::disconnect(c);

        QVERIFY2(switched, "test never observed A's in-flight window");
        QCOMPARE(m_chat->activeConversationId(), b);

        waitConvIdle(a);

        QCOMPARE(assistantRowCount(a), 1);
        bool inA = false;
        for (const Message& m : m_msgSvc->getMessages(a)) {
            if (m.role == QStringLiteral("assistant") && m.content == QStringLiteral("from-A-on-X"))
                inA = true;
        }
        QVERIFY2(inA, "A's background reply did not persist to A");

        QCOMPARE(assistantRowCount(b), 0);
        QCOMPARE(m_providerX->stepsConsumed(), 1);
        QCOMPARE(m_providerY->stepsConsumed(), 0);

        m_chat->sendMessage(QStringLiteral("answer in B"), {});
        waitConvIdle(b);
        QCOMPARE(assistantRowCount(b), 1);
        bool inB = false;
        for (const Message& m : m_msgSvc->getMessages(b)) {
            if (m.role == QStringLiteral("assistant") && m.content == QStringLiteral("from-B-on-Y"))
                inB = true;
        }
        QVERIFY2(inB, "B's reply did not persist to B");
        QCOMPARE(m_providerY->stepsConsumed(), 1);
        QCOMPARE(m_providerX->stepsConsumed(), 1);
        QCOMPARE(assistantRowCount(a), 1);
    }
};

QTEST_MAIN(TestChatCoordinator)
#include "test-chat-coordinator.moc"
