// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "api/llm-interface.h"
#include "models/db-manager.h"
#include "models/llm-config.h"
#include "models/message.h"
#include "services/conversation-service.h"
#include "services/conversation-summarizer.h"
#include "services/message-service.h"
#include "services/model-router.h"
#include "services/settings-service.h"
#include "utils/http-client.h"

#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <memory>
#include <QSignalSpy>
#include <QSqlError>
#include <QSqlQuery>
#include <QUuid>

namespace {

class StubSummaryProvider : public ILLMProvider {
    Q_OBJECT
  public:
    enum class Mode { EmitSummary, EmitError, EmitEmpty, Silent };

    explicit StubSummaryProvider(QObject* parent = nullptr) : ILLMProvider(parent) {}

    QString providerId() const override { return QStringLiteral("stub"); }
    QString displayName() const override { return QStringLiteral("Stub"); }
    QStringList availableModels() override { return {}; }
    int contextWindowFor(const QString&) override { return 0; }
    void refreshModels() override {}
    bool supportsStreaming() const override { return true; }
    bool supportsToolCalling() const override { return false; }
    bool supportsVision() const override { return false; }
    void cancelRequest() override { ++cancelCount; }

    void sendRequest(const LlmRequest& req) override {
        ++sendCount;
        lastRequest = req;
        QTimer::singleShot(0, this, [this]() {
            switch (mode) {
                case Mode::EmitSummary: {
                    LlmChunk c1;
                    c1.delta = QStringLiteral("- Decision: use ");
                    emit chunkReceived(c1);
                    LlmChunk c2;
                    c2.delta = QStringLiteral("SQLite for storage.");
                    emit chunkReceived(c2);
                    emit requestFinished(QStringLiteral("stop"), 12);
                    break;
                }
                case Mode::EmitError:
                    emit requestError(QStringLiteral("stub network error"));
                    break;
                case Mode::EmitEmpty:
                    emit requestFinished(QStringLiteral("stop"), 0);
                    break;
                case Mode::Silent:
                    break;
            }
        });
    }

    Mode mode = Mode::EmitSummary;
    int sendCount = 0;
    int cancelCount = 0;
    LlmRequest lastRequest;
};

}  // namespace

class TestConversationSummarizer : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    QString m_dbPath;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<MessageService> m_msgSvc;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<SettingsService> m_settings;
    std::unique_ptr<ConversationSummarizer> m_sum;

    QString makeConv(const QString& title) { return m_convSvc->createConversation(title); }

    void seedMessages(const QString& convId, int count) {
        for (int i = 0; i < count; ++i) {
            Message m;
            m.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            m.conversationId = convId;
            m.role = (i % 2 == 0) ? QStringLiteral("user") : QStringLiteral("assistant");
            m.memberAlias = (i % 2 == 0) ? QString() : QStringLiteral("Coord");
            m.content = QStringLiteral("Message %1 — substantial content about the project "
                                       "decision discussion so excerpts are non-empty.")
                            .arg(i + 1);
            m.createdAt = QDateTime::currentDateTimeUtc().addSecs(i - count - 1);
            QVERIFY(!m_msgSvc->addMessage(m).isEmpty());
        }
    }

    void addAssistantTurns(const QString& convId, int n) {
        for (int i = 0; i < n; ++i) {
            Message m;
            m.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            m.conversationId = convId;
            m.role = QStringLiteral("assistant");
            m.memberAlias = QStringLiteral("Coord");
            m.content = QStringLiteral("Later assistant turn %1.").arg(i + 1);
            m.createdAt = QDateTime::currentDateTimeUtc().addSecs(i + 1);
            QVERIFY(!m_msgSvc->addMessage(m).isEmpty());
        }
    }

    void addReceipts(const QString& convId, int n) {
        for (int i = 0; i < n; ++i) {
            Message m;
            m.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            m.conversationId = convId;
            m.role = QStringLiteral("system");
            m.content = QStringLiteral("~Dynamic Compact Performed, Reason: Auto~");
            m.createdAt = QDateTime::currentDateTimeUtc().addSecs(200 + i);
            QVERIFY(!m_msgSvc->addMessage(m).isEmpty());
        }
    }

    StubSummaryProvider* installStub(StubSummaryProvider::Mode mode) {
        auto stub = std::make_unique<StubSummaryProvider>();
        stub->mode = mode;
        StubSummaryProvider* raw = stub.get();
        m_sum->setProviderForTest(QStringLiteral("ollama"), std::move(stub));
        return raw;
    }

  private slots:
    void initTestCase() {
        QVERIFY(m_tempDir.isValid());
        m_dbPath = m_tempDir.path() + QStringLiteral("/test.db");
        DbManager::instance().close();
        QFile::remove(m_dbPath);
        QVERIFY2(DbManager::instance().open(m_dbPath), "DbManager::open failed");
        QVERIFY2(DbManager::instance().runMigrations(), "runMigrations failed");

        m_convSvc = std::make_unique<ConversationService>(DbManager::instance(), nullptr);
        m_msgSvc = std::make_unique<MessageService>(DbManager::instance(), nullptr);
        m_router = std::make_unique<ModelRouter>(nullptr);
        m_settings = std::make_unique<SettingsService>(DbManager::instance(), nullptr);
        m_sum = std::make_unique<ConversationSummarizer>(
            DbManager::instance(), *m_msgSvc, *m_convSvc, *m_router, *m_settings, nullptr);

        m_settings->setSummarizationProvider(QStringLiteral("ollama"));
        m_settings->setSummarizationModel(QStringLiteral("stub-model"));
    }

    void cleanupTestCase() {
        m_sum.reset();
        m_settings.reset();
        m_router.reset();
        m_msgSvc.reset();
        m_convSvc.reset();
        DbManager::instance().close();
    }

    void schemaV20_tablePresent() {
        QSqlQuery q(DbManager::instance().db());
        QVERIFY(q.exec(QStringLiteral("SELECT conversation_id, summary_text, last_message_id, "
                                      "       last_message_idx, covered_count, generated_at_ms, "
                                      "       token_count, model_used, trigger_reason, "
                                      "       invalidation_reason "
                                      "  FROM conversation_summaries LIMIT 0")));
    }

    void schemaV20_triggerReasonCheckEnforced() {
        QSqlQuery q(DbManager::instance().db());
        q.prepare(QStringLiteral("INSERT INTO conversation_summaries (conversation_id, "
                                 "summary_text, last_message_id, last_message_idx, "
                                 "covered_count, generated_at_ms, token_count, model_used, "
                                 "trigger_reason) VALUES ('x','s','m',1,1,0,1,'m','bogus')"));
        QVERIFY(!q.exec());
    }

    void summaryFor_missingReturnsNullopt() {
        QVERIFY(!m_sum->summaryFor(QStringLiteral("no-such-conv")).has_value());
    }

    void shouldSummarize_gatesHold() {
        const QString convId = QStringLiteral("h-conv");
        QVERIFY(!m_sum->shouldSummarize(convId, 100, 50, false));
        QVERIFY(!m_sum->shouldSummarize(convId, 30, 50, true));
        QVERIFY(!m_sum->shouldSummarize(convId, 100, 10, true));
        QVERIFY(m_sum->shouldSummarize(convId, 100, 50, true));
    }

    void generateAsync_success_persistsRowAndReceipt() {
        const QString convId = makeConv(QStringLiteral("roundtrip"));
        QVERIFY(!convId.isEmpty());
        seedMessages(convId, 40);

        installStub(StubSummaryProvider::Mode::EmitSummary);
        QSignalSpy ready(m_sum.get(), &ConversationSummarizer::summaryReady);
        QSignalSpy failed(m_sum.get(), &ConversationSummarizer::summaryFailed);

        m_sum->generateAsync(convId, QStringLiteral("auto"));
        QVERIFY(m_sum->busy());
        QTRY_COMPARE(ready.count(), 1);
        QCOMPARE(failed.count(), 0);
        QVERIFY(!m_sum->busy());

        const auto row = m_sum->summaryFor(convId);
        QVERIFY(row.has_value());
        QVERIFY(row->fresh());
        QCOMPARE(row->summaryText, QStringLiteral("- Decision: use SQLite for storage."));
        QCOMPARE(row->triggerReason, QStringLiteral("auto"));
        QCOMPARE(row->coveredCount, 25);
        QVERIFY(row->tokenCount > 0);
        QCOMPARE(row->modelUsed, QStringLiteral("ollama/stub-model"));

        const auto msgs = m_msgSvc->getRecentMessages(convId, 100);
        QVERIFY(!msgs.isEmpty());
        const Message& last = msgs.last();
        QCOMPARE(last.role, QStringLiteral("system"));
        QCOMPARE(last.content, QStringLiteral("~Dynamic Compact Performed, Reason: Auto~"));
    }

    void generateAsync_manualReasonInReceipt() {
        const QString convId = makeConv(QStringLiteral("manual"));
        seedMessages(convId, 40);
        installStub(StubSummaryProvider::Mode::EmitSummary);
        QSignalSpy ready(m_sum.get(), &ConversationSummarizer::summaryReady);
        m_sum->generateAsync(convId, QStringLiteral("manual"));
        QTRY_COMPARE(ready.count(), 1);
        const auto msgs = m_msgSvc->getRecentMessages(convId, 100);
        QCOMPARE(msgs.last().content,
                 QStringLiteral("~Dynamic Compact Performed, Reason: Manual~"));
        const auto row = m_sum->summaryFor(convId);
        QCOMPARE(row->triggerReason, QStringLiteral("manual"));
    }

    void generateAsync_providerError_failsWithoutReceipt() {
        const QString convId = makeConv(QStringLiteral("err"));
        seedMessages(convId, 40);
        installStub(StubSummaryProvider::Mode::EmitError);
        QSignalSpy ready(m_sum.get(), &ConversationSummarizer::summaryReady);
        QSignalSpy failed(m_sum.get(), &ConversationSummarizer::summaryFailed);
        m_sum->generateAsync(convId, QStringLiteral("auto"));
        QTRY_COMPARE(failed.count(), 1);
        QCOMPARE(ready.count(), 0);
        QVERIFY(!m_sum->busy());
        QVERIFY(!m_sum->summaryFor(convId).has_value());
        const auto msgs = m_msgSvc->getRecentMessages(convId, 100);
        QCOMPARE(msgs.last().role, QStringLiteral("assistant"));
    }

    void generateAsync_emptyOutput_fails() {
        const QString convId = makeConv(QStringLiteral("empty"));
        seedMessages(convId, 40);
        installStub(StubSummaryProvider::Mode::EmitEmpty);
        QSignalSpy failed(m_sum.get(), &ConversationSummarizer::summaryFailed);
        m_sum->generateAsync(convId, QStringLiteral("auto"));
        QTRY_COMPARE(failed.count(), 1);
        QVERIFY(!m_sum->busy());
    }

    void generateAsync_busySecondRequestDropped() {
        const QString convId = makeConv(QStringLiteral("busy"));
        seedMessages(convId, 40);
        StubSummaryProvider* stub = installStub(StubSummaryProvider::Mode::EmitSummary);
        QSignalSpy ready(m_sum.get(), &ConversationSummarizer::summaryReady);
        m_sum->generateAsync(convId, QStringLiteral("auto"));
        QVERIFY(m_sum->busy());
        m_sum->generateAsync(convId, QStringLiteral("auto"));
        QTRY_COMPARE(ready.count(), 1);
        QCOMPARE(stub->sendCount, 1);
    }

    void generateAsync_tooFewMessages_fails() {
        const QString convId = makeConv(QStringLiteral("tiny"));
        seedMessages(convId, 5);
        installStub(StubSummaryProvider::Mode::EmitSummary);
        QSignalSpy failed(m_sum.get(), &ConversationSummarizer::summaryFailed);
        m_sum->generateAsync(convId, QStringLiteral("auto"));
        QTRY_COMPARE(failed.count(), 1);
    }

    void invalidate_marksNotFresh_andTriggerRefires() {
        const QString convId = makeConv(QStringLiteral("inval"));
        seedMessages(convId, 40);
        installStub(StubSummaryProvider::Mode::EmitSummary);
        QSignalSpy ready(m_sum.get(), &ConversationSummarizer::summaryReady);
        m_sum->generateAsync(convId, QStringLiteral("auto"));
        QTRY_COMPARE(ready.count(), 1);

        QVERIFY(!m_sum->shouldSummarize(convId, 41, 50, true));

        m_sum->invalidate(convId, QStringLiteral("member_changed"));
        const auto row = m_sum->summaryFor(convId);
        QVERIFY(row.has_value());
        QVERIFY(!row->fresh());
        QCOMPARE(row->invalidationReason, QStringLiteral("member_changed"));
        QVERIFY(m_sum->shouldSummarize(convId, 41, 50, true));
    }

    void generateAsync_setsExplicitContextWindow() {
        const QString convId = makeConv(QStringLiteral("numctx"));
        seedMessages(convId, 40);
        StubSummaryProvider* stub = installStub(StubSummaryProvider::Mode::EmitSummary);
        QSignalSpy ready(m_sum.get(), &ConversationSummarizer::summaryReady);
        m_sum->generateAsync(convId, QStringLiteral("auto"));
        QTRY_COMPARE(ready.count(), 1);
        QCOMPARE(stub->lastRequest.config.contextWindow,
                 ConversationSummarizer::kSummaryContextWindow);
    }

    void generateAsync_rollingCoverageIsCumulative() {
        const QString convId = makeConv(QStringLiteral("rolling"));
        seedMessages(convId, 40);
        StubSummaryProvider* stub = installStub(StubSummaryProvider::Mode::EmitSummary);
        QSignalSpy ready(m_sum.get(), &ConversationSummarizer::summaryReady);

        m_sum->generateAsync(convId, QStringLiteral("auto"));
        QTRY_COMPARE(ready.count(), 1);
        const auto first = m_sum->summaryFor(convId);
        QVERIFY(first.has_value());
        QCOMPARE(first->coveredCount, 25);

        addAssistantTurns(convId, 20);
        m_sum->generateAsync(convId, QStringLiteral("auto"));
        QTRY_COMPARE(ready.count(), 2);
        QVERIFY(stub->lastRequest.messages.size() == 1);
        QVERIFY(
            stub->lastRequest.messages.first().content.contains(QStringLiteral("Summary so far")));
        const auto second = m_sum->summaryFor(convId);
        QVERIFY(second.has_value());
        QVERIFY(second->coveredCount > first->coveredCount);
    }

    void shouldSummarize_staleByAssistantTurns() {
        const QString convId = makeConv(QStringLiteral("staleturns"));
        seedMessages(convId, 40);
        installStub(StubSummaryProvider::Mode::EmitSummary);
        QSignalSpy ready(m_sum.get(), &ConversationSummarizer::summaryReady);
        m_sum->generateAsync(convId, QStringLiteral("auto"));
        QTRY_COMPARE(ready.count(), 1);
        QVERIFY(!m_sum->shouldSummarize(convId, 100, 50, true));
        addAssistantTurns(convId, ConversationSummarizer::kStaleAfterNewMessages);
        QVERIFY(m_sum->shouldSummarize(convId, 100, 50, true));
    }

    void shouldSummarize_receiptsDoNotForceRefresh() {
        const QString convId = makeConv(QStringLiteral("receiptloop"));
        seedMessages(convId, 40);
        installStub(StubSummaryProvider::Mode::EmitSummary);
        QSignalSpy ready(m_sum.get(), &ConversationSummarizer::summaryReady);
        m_sum->generateAsync(convId, QStringLiteral("auto"));
        QTRY_COMPARE(ready.count(), 1);
        addReceipts(convId, 40);
        addAssistantTurns(convId, 2);
        QVERIFY(!m_sum->shouldSummarize(convId, 500, 50, true, 95, 0));
    }

    void shouldSummarize_proactiveFill_firesBeforeAnyDrop() {
        const QString convId = makeConv(QStringLiteral("proactive"));
        seedMessages(convId, 40);
        QVERIFY(m_sum->shouldSummarize(
            convId, 40, 0, true, ConversationSummarizer::kProactiveFillPercent, 0));
        QVERIFY(!m_sum->shouldSummarize(
            convId, 40, 0, true, ConversationSummarizer::kProactiveFillPercent - 1, 0));
    }

    void shouldSummarize_proactiveFill_freshSummaryIsLoopGuard() {
        const QString convId = makeConv(QStringLiteral("loopguard"));
        seedMessages(convId, 40);
        installStub(StubSummaryProvider::Mode::EmitSummary);
        QSignalSpy ready(m_sum.get(), &ConversationSummarizer::summaryReady);
        m_sum->generateAsync(convId, QStringLiteral("auto"));
        QTRY_COMPARE(ready.count(), 1);
        QVERIFY(!m_sum->shouldSummarize(convId, 41, 0, true, 90, 0));
    }

    void shouldSummarize_turnCadence_noSummaryCountsWholeConv() {
        const QString convId = makeConv(QStringLiteral("cadence"));
        seedMessages(convId, 50);
        QVERIFY(m_sum->shouldSummarize(convId, 50, 0, true, 0, 25));
        QVERIFY(!m_sum->shouldSummarize(convId, 50, 0, true, 0, 26));
        QVERIFY(!m_sum->shouldSummarize(convId, 50, 0, true, 0, 0));
    }

    void shouldSummarize_turnCadence_countsAfterCoverage() {
        const QString convId = makeConv(QStringLiteral("cadence2"));
        seedMessages(convId, 40);
        installStub(StubSummaryProvider::Mode::EmitSummary);
        QSignalSpy ready(m_sum.get(), &ConversationSummarizer::summaryReady);
        m_sum->generateAsync(convId, QStringLiteral("auto"));
        QTRY_COMPARE(ready.count(), 1);
        QVERIFY(m_sum->shouldSummarize(convId, 41, 0, true, 0, 8));
        QVERIFY(!m_sum->shouldSummarize(convId, 41, 0, true, 0, 9));
    }

    void coverageBoundary_walksBackOverToolRows() {
        const QString convId = makeConv(QStringLiteral("boundary"));
        for (int i = 0; i < 40; ++i) {
            Message m;
            m.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
            m.conversationId = convId;
            if (i == 25 || i == 26) {
                m.role = QStringLiteral("tool");
                m.content = QStringLiteral("{\"result\": %1}").arg(i);
            } else {
                m.role = (i % 2 == 0) ? QStringLiteral("user") : QStringLiteral("assistant");
                m.memberAlias = (i % 2 == 0) ? QString() : QStringLiteral("Coord");
                m.content =
                    QStringLiteral("Row %1 — substantive project discussion content.").arg(i + 1);
            }
            m.createdAt = QDateTime::currentDateTimeUtc().addSecs(i - 41);
            QVERIFY(!m_msgSvc->addMessage(m).isEmpty());
        }
        installStub(StubSummaryProvider::Mode::EmitSummary);
        QSignalSpy ready(m_sum.get(), &ConversationSummarizer::summaryReady);
        m_sum->generateAsync(convId, QStringLiteral("auto"));
        QTRY_COMPARE(ready.count(), 1);
        const auto row = m_sum->summaryFor(convId);
        QVERIFY(row.has_value());
        QCOMPARE(row->coveredCount, 24);
        QCOMPARE(row->lastMessageIdx, 24);
    }

    void llmConfig_compactEveryTurns_roundTrip() {
        LlmConfig cfg;
        QCOMPARE(cfg.compactEveryTurns, 20);
        cfg.compactEveryTurns = 0;
        const LlmConfig back = LlmConfig::fromJson(cfg.toJson());
        QCOMPARE(back.compactEveryTurns, 0);
        const LlmConfig fresh = LlmConfig::fromJson(QJsonObject{});
        QCOMPARE(fresh.compactEveryTurns, 20);
    }

    void clearFor_deletesRow() {
        const QString convId = makeConv(QStringLiteral("clear"));
        seedMessages(convId, 40);
        installStub(StubSummaryProvider::Mode::EmitSummary);
        QSignalSpy ready(m_sum.get(), &ConversationSummarizer::summaryReady);
        m_sum->generateAsync(convId, QStringLiteral("auto"));
        QTRY_COMPARE(ready.count(), 1);
        QVERIFY(m_sum->summaryFor(convId).has_value());
        m_sum->clearFor(convId);
        QVERIFY(!m_sum->summaryFor(convId).has_value());
    }
};

QTEST_MAIN(TestConversationSummarizer)
#include "test-conversation-summarizer.moc"
