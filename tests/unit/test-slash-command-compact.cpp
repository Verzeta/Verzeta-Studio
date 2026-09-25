// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "api/llm-interface.h"
#include "models/db-manager.h"
#include "models/message.h"
#include "services/conversation-service.h"
#include "services/conversation-summarizer.h"
#include "services/message-service.h"
#include "services/model-router.h"
#include "services/settings-service.h"
#include "services/slash-command-service.h"
#include "utils/http-client.h"

#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <memory>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QUuid>

namespace {

class StubProvider : public ILLMProvider {
    Q_OBJECT
  public:
    explicit StubProvider(QObject* parent = nullptr) : ILLMProvider(parent) {}
    QString providerId() const override { return QStringLiteral("stub"); }
    QString displayName() const override { return QStringLiteral("Stub"); }
    QStringList availableModels() override { return {}; }
    int contextWindowFor(const QString&) override { return 0; }
    void refreshModels() override {}
    bool supportsStreaming() const override { return true; }
    bool supportsToolCalling() const override { return false; }
    bool supportsVision() const override { return false; }
    void cancelRequest() override {}
    void sendRequest(const LlmRequest&) override {
        QTimer::singleShot(0, this, [this]() {
            LlmChunk c;
            c.delta = QStringLiteral("- Summary bullet.");
            emit chunkReceived(c);
            emit requestFinished(QStringLiteral("stop"), 4);
        });
    }
};

}  // namespace

class TestSlashCommandCompact : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<MessageService> m_msgSvc;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<SettingsService> m_settings;
    std::unique_ptr<ConversationSummarizer> m_sum;
    std::unique_ptr<SlashCommandService> m_slash;
    QString m_convId;
    QStringList m_posts;

    void seed(int count) {
        for (int i = 0; i < count; ++i) {
            Message m;
            m.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            m.conversationId = m_convId;
            m.role = (i % 2 == 0) ? QStringLiteral("user") : QStringLiteral("assistant");
            m.content = QStringLiteral("Seed message %1 with enough substance to excerpt.").arg(i);
            m.createdAt = QDateTime::currentDateTimeUtc().addSecs(i - count - 1);
            QVERIFY(!m_msgSvc->addMessage(m).isEmpty());
        }
    }

    SlashCommandContext ctx(const QString& text) {
        SlashCommandContext c;
        c.text = text;
        c.activeConvId = m_convId;
        c.summarizer = m_sum.get();
        ConversationService* convSvc = m_convSvc.get();
        MessageService* msgSvc = m_msgSvc.get();
        ConversationSummarizer* sum = m_sum.get();
        const QString convId = m_convId;
        c.wipeActiveConversation = [convSvc, msgSvc, sum, convId]() -> int {
            const int deleted = msgSvc->deleteAllForConversation(convId);
            if (deleted < 0)
                return -1;
            sum->clearFor(convId);
            convSvc->clearAuxiliaryContent(convId);
            return deleted;
        };
        c.postMessage = [this](const QString& content) { m_posts.append(content); };
        return c;
    }

    int messageCount() { return m_msgSvc->getRecentMessages(m_convId, 1000).size(); }

  private slots:
    void initTestCase() {
        QVERIFY(m_tempDir.isValid());
        const QString dbPath = m_tempDir.path() + QStringLiteral("/test.db");
        DbManager::instance().close();
        QFile::remove(dbPath);
        QVERIFY(DbManager::instance().open(dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_convSvc = std::make_unique<ConversationService>(DbManager::instance(), nullptr);
        m_msgSvc = std::make_unique<MessageService>(DbManager::instance(), nullptr);
        m_router = std::make_unique<ModelRouter>(nullptr);
        m_settings = std::make_unique<SettingsService>(DbManager::instance(), nullptr);
        m_sum = std::make_unique<ConversationSummarizer>(
            DbManager::instance(), *m_msgSvc, *m_convSvc, *m_router, *m_settings, nullptr);
        m_settings->setSummarizationProvider(QStringLiteral("ollama"));
        m_settings->setSummarizationModel(QStringLiteral("stub"));
        m_sum->setProviderForTest(QStringLiteral("ollama"), std::make_unique<StubProvider>());

        m_slash = std::make_unique<SlashCommandService>(nullptr);
        m_convId = m_convSvc->createConversation(QStringLiteral("slash-test"));
        QVERIFY(!m_convId.isEmpty());
        seed(40);
    }

    void cleanupTestCase() {
        m_slash.reset();
        m_sum.reset();
        m_settings.reset();
        m_router.reset();
        m_msgSvc.reset();
        m_convSvc.reset();
        DbManager::instance().close();
    }

    void init() { m_posts.clear(); }

    void compact_triggersManualGeneration() {
        QSignalSpy ready(m_sum.get(), &ConversationSummarizer::summaryReady);
        QVERIFY(m_slash->tryHandle(ctx(QStringLiteral("/compact"))));
        QCOMPARE(m_posts.size(), 1);
        QVERIFY(m_posts.first().contains(QStringLiteral("Compaction started")));
        QTRY_COMPARE(ready.count(), 1);
        QCOMPARE(ready.first().at(1).toString(), QStringLiteral("manual"));
        const auto row = m_sum->summaryFor(m_convId);
        QVERIFY(row.has_value());
        QCOMPARE(row->triggerReason, QStringLiteral("manual"));
        const auto msgs = m_msgSvc->getRecentMessages(m_convId, 1000);
        QCOMPARE(msgs.last().content,
                 QStringLiteral("~Dynamic Compact Performed, Reason: Manual~"));
    }

    void compact_noSummarizerDegradesGracefully() {
        SlashCommandContext c = ctx(QStringLiteral("/compact"));
        c.summarizer = nullptr;
        QVERIFY(m_slash->tryHandle(c));
        QCOMPARE(m_posts.size(), 1);
        QVERIFY(m_posts.first().contains(QStringLiteral("unavailable")));
    }

    void flashmemory_withoutConfirmOnlyWarns() {
        const int before = messageCount();
        QVERIFY(before > 0);
        QVERIFY(m_slash->tryHandle(ctx(QStringLiteral("/flashmemory"))));
        QCOMPARE(m_posts.size(), 1);
        QVERIFY(m_posts.first().contains(QStringLiteral("/flashmemory confirm")));
        QCOMPARE(messageCount(), before);
    }

    void flashmemory_confirmWipesEverything() {
        QVERIFY(messageCount() > 0);
        QVERIFY(m_sum->summaryFor(m_convId).has_value());

        QVERIFY(m_slash->tryHandle(ctx(QStringLiteral("/flashmemory confirm"))));

        QCOMPARE(messageCount(), 0);
        QVERIFY(!m_sum->summaryFor(m_convId).has_value());
        QVERIFY(m_convSvc->getConversation(m_convId).has_value());
        QVERIFY(!m_posts.isEmpty());
        QVERIFY(m_posts.last().contains(QStringLiteral("Memory flashed by user")));
    }
};

QTEST_MAIN(TestSlashCommandCompact)
#include "test-slash-command-compact.moc"
