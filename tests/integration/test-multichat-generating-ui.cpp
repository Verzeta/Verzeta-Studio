// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


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
#include <QSignalSpy>
#include <QSqlDatabase>

class TestMultichatGeneratingUi : public QObject {
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

  private slots:
    void init() {
        m_dbPath = m_tempDir.path() +
                   QStringLiteral("/genui_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
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

    void test_backgroundConversation_readsAsGenerating() {
        const QString a = m_convSvc->createConversation(QStringLiteral("A"));
        const QString b = m_convSvc->createConversation(QStringLiteral("B"));
        pinConvToProvider(a, QStringLiteral("provX"), QStringLiteral("model-x"));
        pinConvToProvider(b, QStringLiteral("provY"), QStringLiteral("model-y"));

        m_providerX->setHoldInFlight(true);
        m_providerY->setHoldInFlight(true);
        m_providerX->setScript({textStep(QStringLiteral("a-reply"))});
        m_providerY->setScript({textStep(QStringLiteral("b-reply"))});

        QSignalSpy genSpy(m_cc.get(), &ChatController::generatingConversationsChanged);

        QVERIFY(!m_cc->isConversationGenerating(a));
        QVERIFY(!m_cc->isConversationGenerating(b));

        m_cc->switchConversation(a);
        m_cc->sendMessage(QStringLiteral("hi A"), {});
        pump();
        QVERIFY2(m_providerX->hasHeldRequest(), "A's request never parked in flight");
        QVERIFY(m_cc->isConversationGenerating(a));
        const int afterAStart = genSpy.count();
        QVERIFY2(afterAStart >= 1, "generatingConversationsChanged did not fire on A start");

        m_cc->switchConversation(b);
        m_cc->sendMessage(QStringLiteral("hi B"), {});
        pump();
        QVERIFY2(m_providerY->hasHeldRequest(), "B's request never parked in flight");

        QVERIFY2(m_cc->isConversationGenerating(a),
                 "background conversation A stopped reading as generating "
                 "after the foreground switched to B");
        QVERIFY2(m_cc->isConversationGenerating(b),
                 "foreground conversation B did not read as generating");
        QVERIFY2(genSpy.count() > afterAStart,
                 "generatingConversationsChanged did not fire on B start");

        QVERIFY(m_cc->isGenerating());

        const int beforeRelease = genSpy.count();
        m_providerX->releaseHeld();
        pump();
        QVERIFY2(!m_cc->isConversationGenerating(a),
                 "A still read as generating after its turn finalized");
        QVERIFY2(m_cc->isConversationGenerating(b),
                 "releasing A wrongly cleared B's generating state");
        QVERIFY2(genSpy.count() > beforeRelease,
                 "generatingConversationsChanged did not fire on A release");

        m_providerY->releaseHeld();
        pump();
        QVERIFY(!m_cc->isConversationGenerating(a));
        QVERIFY(!m_cc->isConversationGenerating(b));
        QVERIFY(!m_cc->isGenerating());
        QVERIFY(!m_sched->anyInFlight());
    }

    void test_emptyAndUnknownConvIds_areNotGenerating() {
        QVERIFY(!m_cc->isConversationGenerating(QString()));
        QVERIFY(!m_cc->isConversationGenerating(QStringLiteral("no-such-conv")));
    }
};

QTEST_MAIN(TestMultichatGeneratingUi)
#include "test-multichat-generating-ui.moc"
