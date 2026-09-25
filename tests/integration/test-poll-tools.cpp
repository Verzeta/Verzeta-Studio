// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/models/db-manager.h"
#include "../../backend/services/chat/cascade-controller.h"
#include "../../backend/services/conversation-service.h"
#include "../../backend/services/model-router.h"
#include "../../backend/services/poll-service.h"
#include "../../backend/tools/polls/poll-tool-deps.h"
#include "../../backend/tools/polls/poll-tools.h"

#include <QTemporaryDir>
#include <QtTest>

#include <QDateTime>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QSqlDatabase>

class TestPollTools : public QObject {
    Q_OBJECT

  private slots:
    void init() {
        QVERIFY(m_dbDir.isValid());
        m_dbPath =
            m_dbDir.path() + QStringLiteral("/test_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(m_dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_router = std::make_unique<ModelRouter>();
        m_convs = std::make_unique<ConversationService>(DbManager::instance());
        m_polls = std::make_unique<PollService>(DbManager::instance());
        m_cascade = std::make_unique<Chat::CascadeController>(*m_convs, *m_router);

        m_convId = m_convs->createConversation("c");
        QVERIFY(!m_convId.isEmpty());

        m_deps.polls = m_polls.get();
        m_deps.cascade = m_cascade.get();
        m_deps.activeConvIdGetter = [this]() { return m_convId; };
    }

    void cleanup() {
        m_cascade.reset();
        m_polls.reset();
        m_convs.reset();
        m_router.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QFile::remove(m_dbPath);
    }


    void test_StartPoll_GroupChat_UsesCascadeAlias() {
        m_cascade->setCurrentResponder("Alice", "agent-x");
        Tools::StartPollTool t(m_deps);
        QJsonObject args;
        args.insert("question", "Ship today?");
        QJsonArray opts;
        opts.append("Yes");
        opts.append("No");
        args.insert("options", opts);
        const auto result = t.invoke(args).toObject();
        QVERIFY2(!result.contains("error"), qPrintable(result.value("error").toString()));
        QCOMPARE(result.value("creator_alias").toString(), QStringLiteral("Alice"));
    }

    void test_StartPoll_OneToOneChat_FallsBackToAssistantAlias() {
        m_cascade->setCurrentResponder("", "agent-only-id");

        Tools::StartPollTool t(m_deps);
        QJsonObject args;
        args.insert("question", "Continue?");
        QJsonArray opts;
        opts.append("Yes");
        opts.append("No");
        args.insert("options", opts);
        const auto result = t.invoke(args).toObject();
        QVERIFY2(!result.contains("error"), qPrintable(result.value("error").toString()));
        QCOMPARE(result.value("creator_alias").toString(), QStringLiteral("assistant"));
        QCOMPARE(result.value("creator_kind").toString(), QStringLiteral("agent"));
    }

    void test_StartPoll_RejectsRanked() {
        m_cascade->setCurrentResponder("Alice", "agent-x");
        Tools::StartPollTool t(m_deps);
        QJsonObject args;
        args.insert("question", "Q");
        QJsonArray opts;
        opts.append("A");
        opts.append("B");
        args.insert("options", opts);
        args.insert("mode", "ranked");
        const auto result = t.invoke(args).toObject();
        QVERIFY(result.contains("error"));
        QVERIFY(result.value("error").toString().contains("ranked"));
    }

    void test_StartPoll_RejectsTooFewOptions() {
        m_cascade->setCurrentResponder("Alice", "agent-x");
        Tools::StartPollTool t(m_deps);
        QJsonObject args;
        args.insert("question", "Q");
        QJsonArray opts;
        opts.append("Only one");
        args.insert("options", opts);
        const auto result = t.invoke(args).toObject();
        QVERIFY(result.contains("error"));
        QVERIFY(result.value("error").toString().contains("at least 2"));
    }

    void test_StartPoll_RejectsTooManyOptions() {
        m_cascade->setCurrentResponder("Alice", "agent-x");
        Tools::StartPollTool t(m_deps);
        QJsonObject args;
        args.insert("question", "Q");
        QJsonArray opts;
        for (int i = 0; i < 11; ++i)
            opts.append(QStringLiteral("opt%1").arg(i));
        args.insert("options", opts);
        const auto result = t.invoke(args).toObject();
        QVERIFY(result.contains("error"));
        QVERIFY(result.value("error").toString().contains("cannot exceed 10"));
    }

    void test_StartPoll_RejectsDuplicateQuestion() {
        m_cascade->setCurrentResponder("Alice", "agent-x");

        Tools::StartPollTool t(m_deps);
        QJsonObject args;
        args.insert("question", "Should we ship today?");
        QJsonArray opts;
        opts.append("Yes");
        opts.append("No");
        args.insert("options", opts);
        const auto first = t.invoke(args).toObject();
        QVERIFY2(!first.contains("error"), qPrintable(first.value("error").toString()));
        const QString firstId = first.value("id").toString();
        QVERIFY(!firstId.isEmpty());

        const auto second = t.invoke(args).toObject();
        QVERIFY(second.contains("error"));
        QVERIFY(second.value("error").toString().contains("already exists"));
        QCOMPARE(second.value("existing_poll_id").toString(), firstId);

        QJsonObject argsB;
        argsB.insert("question", "  should WE Ship Today?  ");
        argsB.insert("options", opts);
        const auto third = t.invoke(argsB).toObject();
        QVERIFY(third.contains("error"));
        QCOMPARE(third.value("existing_poll_id").toString(), firstId);

        QVERIFY(m_polls->closePoll(firstId));
        const auto fourth = t.invoke(args).toObject();
        QVERIFY2(!fourth.contains("error"), qPrintable(fourth.value("error").toString()));
        QVERIFY(!fourth.value("id").toString().isEmpty());
        QVERIFY(fourth.value("id").toString() != firstId);
    }

    void test_StartPoll_RejectsEmptyQuestion() {
        m_cascade->setCurrentResponder("Alice", "agent-x");
        Tools::StartPollTool t(m_deps);
        QJsonObject args;
        args.insert("question", "");
        QJsonArray opts;
        opts.append("A");
        opts.append("B");
        args.insert("options", opts);
        const auto result = t.invoke(args).toObject();
        QVERIFY(result.contains("error"));
        QVERIFY(result.value("error").toString().contains("question"));
    }


    void test_CastVote_OneToOneChat_AssistantVotesByText() {
        const QString pid =
            m_polls->createPoll(m_convId, "user", "user", "Q", {"Yes", "No"}, "single", 0);
        QVERIFY(!pid.isEmpty());

        m_cascade->setCurrentResponder("", "agent-only-id");

        Tools::CastVoteTool t(m_deps);
        QJsonObject args;
        args.insert("poll_id", pid);
        args.insert("option_text", "Yes");
        const auto result = t.invoke(args).toObject();
        QVERIFY2(!result.contains("error"), qPrintable(result.value("error").toString()));

        const auto votes = m_polls->votesForPoll(pid);
        QCOMPARE(votes.size(), 1);
        QCOMPARE(votes.first().voterAlias, QStringLiteral("assistant"));
    }

    void test_CastVote_RejectsMissingBothIdAndText() {
        m_cascade->setCurrentResponder("Alice", "agent-x");
        const QString pid =
            m_polls->createPoll(m_convId, "Alice", "agent", "Q", {"Yes", "No"}, "single", 0);

        Tools::CastVoteTool t(m_deps);
        QJsonObject args;
        args.insert("poll_id", pid);
        const auto result = t.invoke(args).toObject();
        QVERIFY(result.contains("error"));
        QVERIFY(result.value("error").toString().contains("option_id or option_text"));
    }

    void test_CastVote_RejectsMissingPollId() {
        m_cascade->setCurrentResponder("Alice", "agent-x");
        Tools::CastVoteTool t(m_deps);
        QJsonObject args;
        args.insert("option_text", "Yes");
        const auto result = t.invoke(args).toObject();
        QVERIFY(result.contains("error"));
        QVERIFY(result.value("error").toString().contains("poll_id"));
    }


    void test_GetPollResults_NoPollId_ListsConvPolls() {
        const QString p1 =
            m_polls->createPoll(m_convId, "user", "user", "Q1", {"a", "b"}, "single", 0);
        const QString p2 =
            m_polls->createPoll(m_convId, "user", "user", "Q2", {"a", "b"}, "single", 0);

        m_cascade->setCurrentResponder("Alice", "agent-x");
        Tools::GetPollResultsTool t(m_deps);
        const auto result = t.invoke(QJsonObject{}).toObject();
        QCOMPARE(result.value("count").toInt(), 2);
        const auto arr = result.value("polls").toArray();
        QCOMPARE(arr.size(), 2);
    }

    void test_GetPollResults_UnknownPollId_ReturnsError() {
        m_cascade->setCurrentResponder("Alice", "agent-x");
        Tools::GetPollResultsTool t(m_deps);
        QJsonObject args;
        args.insert("poll_id", "not-a-real-id");
        const auto result = t.invoke(args).toObject();
        QVERIFY(result.contains("error"));
    }


    void test_ClosePoll_HappyPath() {
        const QString pid =
            m_polls->createPoll(m_convId, "user", "user", "Q", {"a", "b"}, "single", 0);
        m_cascade->setCurrentResponder("Alice", "agent-x");
        Tools::ClosePollTool t(m_deps);
        QJsonObject args;
        args.insert("poll_id", pid);
        const auto result = t.invoke(args).toObject();
        QVERIFY2(!result.contains("error"), qPrintable(result.value("error").toString()));
        QCOMPARE(result.value("status").toString(), QStringLiteral("closed"));
    }


    void test_ToolNames_Stable() {
        QCOMPARE(Tools::StartPollTool(m_deps).name(), QStringLiteral("start_poll"));
        QCOMPARE(Tools::CastVoteTool(m_deps).name(), QStringLiteral("cast_vote"));
        QCOMPARE(Tools::GetPollResultsTool(m_deps).name(), QStringLiteral("get_poll_results"));
        QCOMPARE(Tools::ClosePollTool(m_deps).name(), QStringLiteral("close_poll"));
    }

    void test_AllRunOnMainThread() {
        QVERIFY(Tools::StartPollTool(m_deps).runsOnMainThread());
        QVERIFY(Tools::CastVoteTool(m_deps).runsOnMainThread());
        QVERIFY(Tools::GetPollResultsTool(m_deps).runsOnMainThread());
        QVERIFY(Tools::ClosePollTool(m_deps).runsOnMainThread());
    }

  private:
    QTemporaryDir m_dbDir;
    QString m_dbPath;
    QString m_convId;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<ConversationService> m_convs;
    std::unique_ptr<PollService> m_polls;
    std::unique_ptr<Chat::CascadeController> m_cascade;
    Tools::PollToolDeps m_deps;
};

QTEST_MAIN(TestPollTools)
#include "test-poll-tools.moc"
