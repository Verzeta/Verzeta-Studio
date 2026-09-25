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

class TestPollToolPerClient : public QObject {
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

        m_localConvId = m_convs->createConversation("local desktop");
        m_wireConvId = m_convs->createConversation("wire android");
        QVERIFY(!m_localConvId.isEmpty());
        QVERIFY(!m_wireConvId.isEmpty());

        m_deps.polls = m_polls.get();
        m_deps.cascade = m_cascade.get();
        m_deps.activeConvIdGetter = [this]() { return m_localConvId; };
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


    void test_startPoll_argsOverrideCapturedConvAndCascadeAlias() {
        m_cascade->setCurrentResponder("LOCAL_Alice", "agent-local-x");

        Tools::StartPollTool t(m_deps);
        QJsonObject args;
        args.insert("question", "Ship today?");
        QJsonArray opts;
        opts.append("Yes");
        opts.append("No");
        args.insert("options", opts);
        args.insert("__caller_conv_id", m_wireConvId);
        args.insert("__caller_agent_alias", QStringLiteral("WIRE_Bob"));

        const auto r = t.invoke(args).toObject();
        QVERIFY2(!r.contains("error"), qPrintable(r.value("error").toString()));

        const QString pollId = r.value("id").toString();
        QVERIFY(!pollId.isEmpty());

        const QVariantList wirePolls = m_polls->pollsForConversation(m_wireConvId, false);
        const QVariantList localPolls = m_polls->pollsForConversation(m_localConvId, false);
        QCOMPARE(wirePolls.size(), 1);
        QVERIFY2(localPolls.isEmpty(), "Poll must NOT be created in LOCAL conv — args id wins");

        const QVariantMap poll = wirePolls.first().toMap();
        QCOMPARE(poll.value("creator_alias").toString(), QStringLiteral("WIRE_Bob"));
    }

    void test_startPoll_fallsBackToCapturedGetterAndCascade_whenArgsEmpty() {
        m_cascade->setCurrentResponder("LOCAL_Alice", "agent-local-x");

        Tools::StartPollTool t(m_deps);
        QJsonObject args;
        args.insert("question", "Continue?");
        QJsonArray opts;
        opts.append("Yes");
        opts.append("No");
        args.insert("options", opts);

        const auto r = t.invoke(args).toObject();
        QVERIFY2(!r.contains("error"), qPrintable(r.value("error").toString()));

        const QVariantList localPolls = m_polls->pollsForConversation(m_localConvId, false);
        QCOMPARE(localPolls.size(), 1);
        QCOMPARE(localPolls.first().toMap().value("creator_alias").toString(),
                 QStringLiteral("LOCAL_Alice"));
    }


    void test_castVote_argsAliasWins_overCascadeAlias() {
        m_cascade->setCurrentResponder("LOCAL_Alice", "agent-local-x");

        const QString pollId =
            m_polls->createPoll(m_wireConvId,
                                QStringLiteral("Bob"),
                                QStringLiteral("agent"),
                                QStringLiteral("Ship?"),
                                QStringList{QStringLiteral("Yes"), QStringLiteral("No")},
                                QStringLiteral("single"),
                                0);
        QVERIFY(!pollId.isEmpty());

        const QVariantMap results = m_polls->pollResults(pollId);
        const QVariantList optList = results.value("options").toList();
        QVERIFY(optList.size() >= 1);
        const QString yesOptId = optList.first().toMap().value("id").toString();
        QVERIFY(!yesOptId.isEmpty());

        Tools::CastVoteTool t(m_deps);
        QJsonObject args;
        args.insert("poll_id", pollId);
        args.insert("option_id", yesOptId);
        args.insert("__caller_agent_alias", QStringLiteral("WIRE_Bob"));

        const auto r = t.invoke(args).toObject();
        QVERIFY2(!r.contains("error"), qPrintable(r.value("error").toString()));

        const auto votes = m_polls->votesForPoll(pollId);
        QCOMPARE(votes.size(), 1);
        QCOMPARE(votes.first().voterAlias, QStringLiteral("WIRE_Bob"));
    }

    void test_castVote_fallsBackToCascadeAlias_whenArgsEmpty() {
        m_cascade->setCurrentResponder("LOCAL_Alice", "agent-local-x");

        const QString pollId =
            m_polls->createPoll(m_localConvId,
                                QStringLiteral("Charlie"),
                                QStringLiteral("agent"),
                                QStringLiteral("Continue?"),
                                QStringList{QStringLiteral("Yes"), QStringLiteral("No")},
                                QStringLiteral("single"),
                                0);
        QVERIFY(!pollId.isEmpty());

        const QVariantMap r0 = m_polls->pollResults(pollId);
        const QString yesOptId =
            r0.value("options").toList().first().toMap().value("id").toString();

        Tools::CastVoteTool t(m_deps);
        QJsonObject args;
        args.insert("poll_id", pollId);
        args.insert("option_id", yesOptId);

        const auto r = t.invoke(args).toObject();
        QVERIFY2(!r.contains("error"), qPrintable(r.value("error").toString()));

        const auto votes = m_polls->votesForPoll(pollId);
        QCOMPARE(votes.size(), 1);
        QCOMPARE(votes.first().voterAlias, QStringLiteral("LOCAL_Alice"));
    }


    void test_castVote_assistantFallback_whenBothEmpty() {
        m_cascade->setCurrentResponder(QString(), "agent-only-id");

        const QString pollId =
            m_polls->createPoll(m_localConvId,
                                QStringLiteral("assistant"),
                                QStringLiteral("agent"),
                                QStringLiteral("Continue?"),
                                QStringList{QStringLiteral("Yes"), QStringLiteral("No")},
                                QStringLiteral("single"),
                                0);
        QVERIFY(!pollId.isEmpty());

        const QString yesOptId = m_polls->pollResults(pollId)
                                     .value("options")
                                     .toList()
                                     .first()
                                     .toMap()
                                     .value("id")
                                     .toString();

        Tools::CastVoteTool t(m_deps);
        QJsonObject args;
        args.insert("poll_id", pollId);
        args.insert("option_id", yesOptId);

        const auto r = t.invoke(args).toObject();
        QVERIFY2(!r.contains("error"), qPrintable(r.value("error").toString()));

        const auto votes = m_polls->votesForPoll(pollId);
        QCOMPARE(votes.size(), 1);
        QCOMPARE(votes.first().voterAlias, QStringLiteral("assistant"));
    }


    void test_getPollResults_argsConvIdOverridesCapturedGetter() {
        m_cascade->setCurrentResponder("Alice", "agent-x");

        const QString pollId =
            m_polls->createPoll(m_wireConvId,
                                QStringLiteral("Alice"),
                                QStringLiteral("agent"),
                                QStringLiteral("Wire-side Q?"),
                                QStringList{QStringLiteral("A"), QStringLiteral("B")},
                                QStringLiteral("single"),
                                0);
        QVERIFY(!pollId.isEmpty());

        Tools::GetPollResultsTool t(m_deps);
        QJsonObject args;
        args.insert("__caller_conv_id", m_wireConvId);

        const auto r = t.invoke(args).toObject();
        QVERIFY2(!r.contains("error"), qPrintable(r.value("error").toString()));

        const QJsonArray polls = r.value("polls").toArray();
        QCOMPARE(polls.size(), 1);
        QCOMPARE(polls.first().toObject().value("question").toString(),
                 QStringLiteral("Wire-side Q?"));
    }

  private:
    QTemporaryDir m_dbDir;
    QString m_dbPath;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<ConversationService> m_convs;
    std::unique_ptr<PollService> m_polls;
    std::unique_ptr<Chat::CascadeController> m_cascade;
    QString m_localConvId;
    QString m_wireConvId;
    Tools::PollToolDeps m_deps;
};

QTEST_MAIN(TestPollToolPerClient)
#include "test-poll-tool-per-client.moc"
