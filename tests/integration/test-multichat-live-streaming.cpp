// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/models/db-manager.h"
#include "../../backend/models/llm-config.h"
#include "../../backend/models/message-list-model.h"
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
#include <QSignalSpy>
#include <QSqlDatabase>

class TestMultichatLiveStreaming : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    QString m_dbPath;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<MessageService> m_msgSvc;
    std::unique_ptr<ExportService> m_exportSvc;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<Chat::ProviderScheduler> m_sched;
    std::unique_ptr<ChatController> m_cc;
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

    MessageListModel* view() const { return m_cc->messages(); }

    QString visibleAssistantContent() const {
        MessageListModel* m = view();
        QString out;
        for (int i = 0; i < m->count(); ++i) {
            const QModelIndex idx = m->index(i);
            if (m->data(idx, MessageListModel::RoleRole).toString() ==
                QStringLiteral("assistant")) {
                out += m->data(idx, MessageListModel::ContentRole).toString();
            }
        }
        return out;
    }

    int visibleAssistantRowCount() const {
        MessageListModel* m = view();
        int n = 0;
        for (int i = 0; i < m->count(); ++i) {
            const QModelIndex idx = m->index(i);
            if (m->data(idx, MessageListModel::RoleRole).toString() == QStringLiteral("assistant"))
                ++n;
        }
        return n;
    }

    int persistedAssistantRowCount(const QString& convId) const {
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
                   QStringLiteral("/live_stream_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
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

        m_cc = std::make_unique<ChatController>(*m_router, *m_convSvc, *m_msgSvc, *m_exportSvc);
        m_cc->setProviderScheduler(m_sched.get());
    }

    void cleanup() {
        m_cc.reset();
        m_sched.reset();
        m_router.reset();
        m_exportSvc.reset();
        m_msgSvc.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QFile::remove(m_dbPath);
    }

    void test_backgroundStream_doesNotMutateActiveView() {
        const QString a = m_convSvc->createConversation(QStringLiteral("A"));
        const QString b = m_convSvc->createConversation(QStringLiteral("B"));
        pinConvToProvider(a, QStringLiteral("provX"), QStringLiteral("model-x"));
        pinConvToProvider(b, QStringLiteral("provY"), QStringLiteral("model-y"));

        m_providerX->setHoldInFlight(true);
        m_providerY->setHoldInFlight(true);
        m_providerX->setScript({textStep(QStringLiteral("a-stream"))});
        m_providerY->setScript({textStep(QStringLiteral("b-stream"))});

        m_cc->switchConversation(a);
        m_cc->sendMessage(QStringLiteral("hi A"), {});
        pump();
        QVERIFY2(m_providerX->hasHeldRequest(), "A never parked in flight");
        QCOMPARE(view()->activeConversationId(), a);
        QVERIFY2(view()->hasStreamingMessage(),
                 "A's in-flight placeholder is not visible while on A");
        QTRY_COMPARE(visibleAssistantContent(), QStringLiteral("a-stream"));

        m_cc->switchConversation(b);
        m_cc->sendMessage(QStringLiteral("hi B"), {});
        pump();
        QVERIFY2(m_providerY->hasHeldRequest(), "B never parked in flight");

        QCOMPARE(view()->activeConversationId(), b);
        QTRY_COMPARE(visibleAssistantContent(), QStringLiteral("b-stream"));
        const QString shown = visibleAssistantContent();
        QVERIFY2(!shown.contains(QStringLiteral("a-stream")),
                 "background conversation A's stream bled into B's view");
        QCOMPARE(visibleAssistantRowCount(), 1);

        pump();
        QCOMPARE(visibleAssistantContent(), QStringLiteral("b-stream"));
        QCOMPARE(visibleAssistantRowCount(), 1);

        m_providerX->releaseHeld();
        m_providerY->releaseHeld();
        pump();
        QVERIFY(!m_sched->anyInFlight());
    }

    void test_switchToMidStreamConv_adoptsLivePlaceholder_finalizesOnce() {
        const QString a = m_convSvc->createConversation(QStringLiteral("A"));
        const QString b = m_convSvc->createConversation(QStringLiteral("B"));
        pinConvToProvider(a, QStringLiteral("provX"), QStringLiteral("model-x"));
        pinConvToProvider(b, QStringLiteral("provY"), QStringLiteral("model-y"));

        m_providerX->setHoldInFlight(true);
        m_providerY->setHoldInFlight(true);
        m_providerX->setScript({textStep(QStringLiteral("alpha-reply"))});
        m_providerY->setScript({textStep(QStringLiteral("beta-reply"))});

        m_cc->switchConversation(a);
        m_cc->sendMessage(QStringLiteral("hi A"), {});
        pump();
        QVERIFY2(m_providerX->hasHeldRequest(), "A never parked in flight");
        m_cc->switchConversation(b);
        m_cc->sendMessage(QStringLiteral("hi B"), {});
        pump();
        QVERIFY2(m_providerY->hasHeldRequest(), "B never parked in flight");
        QTRY_COMPARE(visibleAssistantContent(), QStringLiteral("beta-reply"));

        m_cc->switchConversation(a);
        pump();
        QCOMPARE(view()->activeConversationId(), a);
        QVERIFY2(view()->hasStreamingMessage(),
                 "A's in-flight placeholder was lost on switch-back");
        QCOMPARE(visibleAssistantContent(), QStringLiteral("alpha-reply"));
        QCOMPARE(visibleAssistantRowCount(), 1);

        m_providerX->releaseHeld();
        pump();
        QVERIFY2(!view()->hasStreamingMessage(), "A still streaming after release");
        QCOMPARE(visibleAssistantContent(), QStringLiteral("alpha-reply"));
        QCOMPARE(visibleAssistantRowCount(), 1);
        QCOMPARE(persistedAssistantRowCount(a), 1);

        m_providerY->releaseHeld();
        pump();
        QCOMPARE(persistedAssistantRowCount(b), 1);
        QVERIFY(!m_sched->anyInFlight());

        QCOMPARE(view()->activeConversationId(), a);
        QCOMPARE(visibleAssistantContent(), QStringLiteral("alpha-reply"));
        QCOMPARE(visibleAssistantRowCount(), 1);
    }

    void test_singleConversation_streamShownAndFinalizedOnce() {
        const QString a = m_convSvc->createConversation(QStringLiteral("A"));
        pinConvToProvider(a, QStringLiteral("provX"), QStringLiteral("model-x"));

        m_providerX->setHoldInFlight(true);
        m_providerX->setScript({textStep(QStringLiteral("solo-reply"))});

        m_cc->switchConversation(a);
        m_cc->sendMessage(QStringLiteral("hi"), {});
        pump();
        QVERIFY(m_providerX->hasHeldRequest());
        QVERIFY(view()->hasStreamingMessage());
        QTRY_COMPARE(visibleAssistantContent(), QStringLiteral("solo-reply"));
        QCOMPARE(visibleAssistantRowCount(), 1);

        m_providerX->releaseHeld();
        pump();
        QVERIFY(!view()->hasStreamingMessage());
        QCOMPARE(visibleAssistantContent(), QStringLiteral("solo-reply"));
        QCOMPARE(visibleAssistantRowCount(), 1);
        QCOMPARE(persistedAssistantRowCount(a), 1);
        QVERIFY(!m_sched->anyInFlight());
    }
};

QTEST_MAIN(TestMultichatLiveStreaming)
#include "test-multichat-live-streaming.moc"
