// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "api/llm-interface.h"
#include "models/db-manager.h"
#include "services/model-router.h"
#include "services/settings-service.h"
#include "services/subagent-run-service.h"
#include "services/tool-service.h"
#include "utils/http-client.h"

#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <memory>
#include <QSignalSpy>
#include <QSqlQuery>
#include <QUuid>

namespace {

class StubRunProvider : public ILLMProvider {
    Q_OBJECT
  public:
    struct Turn {
        QString content;
        QJsonArray toolCalls;
        QString error;
    };

    explicit StubRunProvider(QObject* parent = nullptr) : ILLMProvider(parent) {}
    QString providerId() const override { return QStringLiteral("stub"); }
    QString displayName() const override { return QStringLiteral("Stub"); }
    QStringList availableModels() override { return {}; }
    int contextWindowFor(const QString&) override { return 0; }
    void refreshModels() override {}
    bool supportsStreaming() const override { return true; }
    bool supportsToolCalling() const override { return true; }
    bool supportsVision() const override { return false; }
    void cancelRequest() override { ++cancelCount; }

    void sendRequest(const LlmRequest& req) override {
        ++sendCount;
        lastRequest = req;
        Turn t;
        if (!script.isEmpty())
            t = script.takeFirst();
        QTimer::singleShot(0, this, [this, t]() {
            if (!t.error.isEmpty()) {
                emit requestError(t.error);
                return;
            }
            if (!t.content.isEmpty()) {
                LlmChunk c;
                c.delta = t.content;
                emit chunkReceived(c);
            }
            for (const QJsonValue& v : t.toolCalls) {
                LlmChunk c;
                c.toolCallJson = v.toObject();
                emit chunkReceived(c);
            }
            emit requestFinished(
                t.toolCalls.isEmpty() ? QStringLiteral("stop") : QStringLiteral("tool_calls"), 7);
        });
    }

    QList<Turn> script;
    int sendCount = 0;
    int cancelCount = 0;
    LlmRequest lastRequest;
};

}  // namespace

class TestSubagentRunService : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    std::unique_ptr<ToolService> m_toolSvc;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<SettingsService> m_settings;
    std::unique_ptr<SubagentRunService> m_svc;

    StubRunProvider* installStub(QList<StubRunProvider::Turn> script) {
        auto stub = std::make_unique<StubRunProvider>();
        stub->script = std::move(script);
        StubRunProvider* raw = stub.get();
        m_svc->setProviderForTest(QStringLiteral("ollama"), std::move(stub));
        return raw;
    }

  private slots:
    void initTestCase() {
        QVERIFY(m_tempDir.isValid());
        const QString dbPath = m_tempDir.path() + QStringLiteral("/test.db");
        DbManager::instance().close();
        QFile::remove(dbPath);
        QVERIFY(DbManager::instance().open(dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_toolSvc = std::make_unique<ToolService>(nullptr);
        ToolSchema timeSchema;
        timeSchema.name = QStringLiteral("get_current_time");
        timeSchema.description = QStringLiteral("Returns the time");
        m_toolSvc->registerTool(timeSchema, [](const QJsonObject&) -> QJsonValue {
            return QJsonObject{{QStringLiteral("time"), QStringLiteral("2026-06-12T00:00:00Z")}};
        });
        m_router = std::make_unique<ModelRouter>(nullptr);
        m_settings = std::make_unique<SettingsService>(DbManager::instance(), nullptr);
        m_svc = std::make_unique<SubagentRunService>(
            DbManager::instance(), *m_toolSvc, *m_router, *m_settings, nullptr);
    }

    void cleanupTestCase() {
        m_svc.reset();
        m_settings.reset();
        m_router.reset();
        m_toolSvc.reset();
        DbManager::instance().close();
    }

    void schemaV21_tablesPresent() {
        QSqlQuery q(DbManager::instance().db());
        QVERIFY(q.exec(QStringLiteral("SELECT id, conversation_id, status FROM subagent_runs "
                                      "LIMIT 0")));
        QVERIFY(q.exec(QStringLiteral("SELECT id, run_id, seq, role FROM subagent_messages "
                                      "LIMIT 0")));
    }

    void spawn_persistsWithDefaults_andStripsRecursion() {
        installStub({{QStringLiteral("final report"), {}, {}}});
        QSignalSpy fin(m_svc.get(), &SubagentRunService::runFinished);
        const QString runId =
            m_svc->spawn(QStringLiteral("conv-1"),
                         QStringLiteral("msg-1"),
                         QStringLiteral("Coord"),
                         QStringLiteral("research X"),
                         {QStringLiteral("get_current_time"), QStringLiteral("spawn_subagent")},
                         QStringLiteral("ollama"),
                         QStringLiteral("stub-model"));
        QVERIFY(!runId.isEmpty());
        QTRY_COMPARE(fin.count(), 1);

        const auto r = m_svc->run(runId);
        QVERIFY(r.has_value());
        QCOMPARE(r->status, QStringLiteral("done"));
        QCOMPARE(r->resultText, QStringLiteral("final report"));
        QVERIFY(!r->toolsWhitelist.contains(QStringLiteral("spawn_subagent")));
        QVERIFY(r->toolsWhitelist.contains(QStringLiteral("get_current_time")));

        const QJsonArray t = m_svc->transcript(runId);
        QCOMPARE(t.size(), 3);
        QCOMPARE(t.at(0).toObject().value(QStringLiteral("role")).toString(),
                 QStringLiteral("system"));
        QCOMPARE(t.at(2).toObject().value(QStringLiteral("content")).toString(),
                 QStringLiteral("final report"));
    }

    void spawn_emptyWhitelistGetsReadOnlyDefault() {
        installStub({{QStringLiteral("ok"), {}, {}}});
        QSignalSpy fin(m_svc.get(), &SubagentRunService::runFinished);
        const QString runId = m_svc->spawn(QStringLiteral("conv-2"),
                                           {},
                                           QStringLiteral("Coord"),
                                           QStringLiteral("task"),
                                           {},
                                           QStringLiteral("ollama"),
                                           QStringLiteral("stub-model"));
        QTRY_COMPARE(fin.count(), 1);
        const auto r = m_svc->run(runId);
        QVERIFY(r->toolsWhitelist.contains(QStringLiteral("read_file")));
        QVERIFY(r->toolsWhitelist.contains(QStringLiteral("search_web")));
        QVERIFY(!r->toolsWhitelist.contains(QStringLiteral("write_file")));
    }

    void run_toolRound_executesWhitelistedTool() {
        const QJsonObject call{
            {QStringLiteral("id"), QStringLiteral("c1")},
            {QStringLiteral("name"), QStringLiteral("get_current_time")},
            {QStringLiteral("arguments"), QJsonObject{}},
        };
        installStub({
            {QStringLiteral("checking the time"), QJsonArray{call}, {}},
            {QStringLiteral("it is 2026"), {}, {}},
        });
        QSignalSpy fin(m_svc.get(), &SubagentRunService::runFinished);
        const QString runId = m_svc->spawn(QStringLiteral("conv-3"),
                                           {},
                                           QStringLiteral("Coord"),
                                           QStringLiteral("what time is it"),
                                           {QStringLiteral("get_current_time")},
                                           QStringLiteral("ollama"),
                                           QStringLiteral("stub-model"));
        QTRY_COMPARE(fin.count(), 1);
        const auto r = m_svc->run(runId);
        QCOMPARE(r->status, QStringLiteral("done"));
        QCOMPARE(r->resultText, QStringLiteral("it is 2026"));
        const QJsonArray t = m_svc->transcript(runId);
        bool sawToolRow = false;
        for (const QJsonValue& v : t) {
            if (v.toObject().value(QStringLiteral("role")).toString() == QStringLiteral("tool")) {
                sawToolRow = true;
                QCOMPARE(v.toObject().value(QStringLiteral("tool_name")).toString(),
                         QStringLiteral("get_current_time"));
            }
        }
        QVERIFY(sawToolRow);
    }

    void run_nonWhitelistedTool_getsErrorResult() {
        const QJsonObject call{
            {QStringLiteral("id"), QStringLiteral("c1")},
            {QStringLiteral("name"), QStringLiteral("write_file")},
            {QStringLiteral("arguments"), QJsonObject{}},
        };
        installStub({
            {QString(), QJsonArray{call}, {}},
            {QStringLiteral("done without writing"), {}, {}},
        });
        QSignalSpy fin(m_svc.get(), &SubagentRunService::runFinished);
        const QString runId = m_svc->spawn(QStringLiteral("conv-4"),
                                           {},
                                           QStringLiteral("Coord"),
                                           QStringLiteral("try writing"),
                                           {QStringLiteral("get_current_time")},
                                           QStringLiteral("ollama"),
                                           QStringLiteral("stub-model"));
        QTRY_COMPARE(fin.count(), 1);
        const QJsonArray t = m_svc->transcript(runId);
        bool sawWhitelistError = false;
        for (const QJsonValue& v : t) {
            if (v.toObject()
                    .value(QStringLiteral("content"))
                    .toString()
                    .contains(QStringLiteral("tool not in whitelist"))) {
                sawWhitelistError = true;
            }
        }
        QVERIFY(sawWhitelistError);
        QCOMPARE(m_svc->run(runId)->status, QStringLiteral("done"));
    }

    void run_providerError_failsRun() {
        installStub({{{}, {}, QStringLiteral("boom")}});
        QSignalSpy fin(m_svc.get(), &SubagentRunService::runFinished);
        const QString runId = m_svc->spawn(QStringLiteral("conv-5"),
                                           {},
                                           QStringLiteral("Coord"),
                                           QStringLiteral("task"),
                                           {},
                                           QStringLiteral("ollama"),
                                           QStringLiteral("stub-model"));
        QTRY_COMPARE(fin.count(), 1);
        const auto r = m_svc->run(runId);
        QCOMPARE(r->status, QStringLiteral("failed"));
        QCOMPARE(r->failReason, QStringLiteral("boom"));
        QCOMPARE(fin.first().at(3).toString(), QStringLiteral("failed"));
    }

    void cancel_queuedRun_neverDispatches() {
        QVERIFY(!m_svc->cancel(QStringLiteral("no-such-run")));
    }
};

QTEST_MAIN(TestSubagentRunService)
#include "test-subagent-run-service.moc"
