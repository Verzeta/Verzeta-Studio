// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/models/db-manager.h"
#include "../../backend/models/message.h"
#include "../../backend/services/conversation-service.h"
#include "../../backend/services/message-service.h"
#include "../../backend/services/poll-service.h"

#include <QTemporaryDir>
#include <QThread>
#include <QtTest>

#include <QDateTime>
#include <QFile>
#include <QSignalSpy>
#include <QSqlDatabase>

class TestPollService : public QObject {
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

        m_convs = std::make_unique<ConversationService>(DbManager::instance());
        m_msgs = std::make_unique<MessageService>(DbManager::instance());
        m_svc = std::make_unique<PollService>(DbManager::instance());

        m_convId = m_convs->createConversation("c");
        QVERIFY(!m_convId.isEmpty());
    }

    void cleanup() {
        m_svc.reset();
        m_msgs.reset();
        m_convs.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QFile::remove(m_dbPath);
    }


    void test_CreatePoll_HappyPath() {
        QSignalSpy spyCreated(m_svc.get(), &PollService::pollCreated);
        const QString pid = m_svc->createPoll(
            m_convId, "Alice", "agent", "Ship today?", {"Yes", "No"}, "single", 0);
        QVERIFY(!pid.isEmpty());
        QCOMPARE(spyCreated.count(), 1);
        QCOMPARE(spyCreated.first().at(0).toString(), m_convId);
        QCOMPARE(spyCreated.first().at(1).toString(), pid);

        const Poll p = m_svc->pollById(pid);
        QCOMPARE(p.question, QStringLiteral("Ship today?"));
        QCOMPARE(p.mode, QStringLiteral("single"));
        QCOMPARE(p.status, QStringLiteral("open"));
        QCOMPARE(m_svc->optionsForPoll(pid).size(), 2);
    }

    void test_CreatePoll_RejectsRanked() {
        const QString pid =
            m_svc->createPoll(m_convId, "Alice", "agent", "Q", {"A", "B"}, "ranked", 0);
        QVERIFY(pid.isEmpty());
    }

    void test_CreatePoll_RejectsTooFewOptions() {
        const QString pid =
            m_svc->createPoll(m_convId, "Alice", "agent", "Q", {"only one"}, "single", 0);
        QVERIFY(pid.isEmpty());
    }

    void test_CreatePoll_RejectsTooManyOptions() {
        QStringList eleven;
        for (int i = 0; i < 11; ++i)
            eleven << QStringLiteral("opt%1").arg(i);
        const QString pid = m_svc->createPoll(m_convId, "Alice", "agent", "Q", eleven, "single", 0);
        QVERIFY(pid.isEmpty());
    }

    void test_CreatePoll_DedupOptionsCaseInsensitive() {
        const QString pid = m_svc->createPoll(
            m_convId, "Alice", "agent", "Q", {"Yes", "yes", "  Yes  ", "No"}, "single", 0);
        QVERIFY(!pid.isEmpty());
        QCOMPARE(m_svc->optionsForPoll(pid).size(), 2);
    }

    void test_CreatePoll_RejectsEmptyQuestion() {
        QVERIFY(
            m_svc->createPoll(m_convId, "A", "agent", "   ", {"a", "b"}, "single", 0).isEmpty());
    }

    void test_CreatePoll_RejectsBadKind() {
        QVERIFY(m_svc->createPoll(m_convId, "A", "robot", "Q", {"a", "b"}, "single", 0).isEmpty());
    }


    void test_Vote_ByOptionId_HappyPath() {
        const QString pid =
            m_svc->createPoll(m_convId, "A", "agent", "Q", {"Yes", "No"}, "single", 0);
        const auto opts = m_svc->optionsForPoll(pid);
        QVERIFY(m_svc->castVoteByOptionId(pid, "Bob", "agent", opts.at(0).id));
        QCOMPARE(m_svc->votesForPoll(pid).size(), 1);
    }

    void test_Vote_ByOptionText_CaseInsensitive() {
        const QString pid =
            m_svc->createPoll(m_convId, "A", "agent", "Q", {"Yes", "No"}, "single", 0);
        QVERIFY(m_svc->castVoteByOptionText(pid, "Bob", "agent", "yes"));
        const auto opts = m_svc->optionsForPoll(pid);
        const auto votes = m_svc->votesForPoll(pid);
        QCOMPARE(votes.size(), 1);
        QCOMPARE(votes.first().optionId, opts.at(0).id);
    }

    void test_Vote_UnknownOptionText_Refused() {
        const QString pid =
            m_svc->createPoll(m_convId, "A", "agent", "Q", {"Yes", "No"}, "single", 0);
        QVERIFY(!m_svc->castVoteByOptionText(pid, "Bob", "agent", "maybe"));
    }

    void test_Vote_SingleMode_OverwritesPriorVote() {
        const QString pid =
            m_svc->createPoll(m_convId, "A", "agent", "Q", {"Yes", "No"}, "single", 0);
        const auto opts = m_svc->optionsForPoll(pid);
        QVERIFY(m_svc->castVoteByOptionId(pid, "Bob", "agent", opts.at(0).id));
        QVERIFY(m_svc->castVoteByOptionId(pid, "Bob", "agent", opts.at(1).id));
        const auto votes = m_svc->votesForPoll(pid);
        QCOMPARE(votes.size(), 1);
        QCOMPARE(votes.first().optionId, opts.at(1).id);
    }

    void test_Vote_MultiMode_AllowsMultipleSelections() {
        const QString pid =
            m_svc->createPoll(m_convId, "A", "agent", "Q", {"A", "B", "C"}, "multi", 0);
        const auto opts = m_svc->optionsForPoll(pid);
        QVERIFY(m_svc->castVoteByOptionId(pid, "Bob", "agent", opts.at(0).id));
        QVERIFY(m_svc->castVoteByOptionId(pid, "Bob", "agent", opts.at(1).id));
        QCOMPARE(m_svc->votesForPoll(pid).size(), 2);
    }

    void test_Vote_RefusesAfterClose() {
        const QString pid =
            m_svc->createPoll(m_convId, "A", "agent", "Q", {"Yes", "No"}, "single", 0);
        QVERIFY(m_svc->closePoll(pid));
        const auto opts = m_svc->optionsForPoll(pid);
        QVERIFY(!m_svc->castVoteByOptionId(pid, "Bob", "agent", opts.at(0).id));
    }


    void test_PollResults_IncludesPerOptionTallyAndWinner() {
        const QString pid =
            m_svc->createPoll(m_convId, "A", "agent", "Q", {"Yes", "No"}, "single", 0);
        const auto opts = m_svc->optionsForPoll(pid);
        QVERIFY(m_svc->castVoteByOptionId(pid, "Bob", "agent", opts.at(0).id));
        QVERIFY(m_svc->castVoteByOptionId(pid, "Carol", "agent", opts.at(0).id));
        QVERIFY(m_svc->castVoteByOptionId(pid, "user", "user", opts.at(1).id));

        const auto m = m_svc->pollResults(pid);
        QCOMPARE(m.value("total_votes").toInt(), 3);
        QCOMPARE(m.value("voter_count").toInt(), 3);
        const auto winners = m.value("winning_option_ids").toStringList();
        QCOMPARE(winners.size(), 1);
        QCOMPARE(winners.first(), opts.at(0).id);
    }


    void test_ClosePoll_FiresClosedSignal() {
        QSignalSpy spyClosed(m_svc.get(), &PollService::pollClosed);
        const QString pid =
            m_svc->createPoll(m_convId, "A", "agent", "Q", {"Yes", "No"}, "single", 0);
        QVERIFY(m_svc->closePoll(pid));
        QCOMPARE(spyClosed.count(), 1);
        QCOMPARE(m_svc->pollById(pid).status, QStringLiteral("closed"));
    }

    void test_ClosePoll_Idempotent() {
        const QString pid =
            m_svc->createPoll(m_convId, "A", "agent", "Q", {"Yes", "No"}, "single", 0);
        QVERIFY(m_svc->closePoll(pid));
        QVERIFY(m_svc->closePoll(pid));
    }

    void test_LazyAutoClose_TriggersOnResults() {
        const QString pid =
            m_svc->createPoll(m_convId, "A", "agent", "Q", {"Yes", "No"}, "single", 0);
        QSqlQuery q(DbManager::instance().db());
        q.prepare(QStringLiteral("UPDATE polls SET closes_at=? WHERE id=?"));
        q.addBindValue(QDateTime::currentDateTimeUtc().addSecs(-60).toString(Qt::ISODateWithMs));
        q.addBindValue(pid);
        QVERIFY(q.exec());

        QSignalSpy spyClosed(m_svc.get(), &PollService::pollClosed);
        const auto m = m_svc->pollResults(pid);
        QCOMPARE(m.value("status").toString(), QStringLiteral("closed"));
        QCOMPARE(spyClosed.count(), 1);
    }


    void test_AutoPersistPollMessage_WhenMessageServiceAttached() {
        const QString pidA =
            m_svc->createPoll(m_convId, "A", "agent", "Q1", {"a", "b"}, "single", 0);
        QVERIFY(!pidA.isEmpty());
        auto messagesBefore = m_msgs->getMessages(m_convId);
        QCOMPARE(messagesBefore.size(), 0);

        m_svc->setMessageService(m_msgs.get());
        const QString pidB =
            m_svc->createPoll(m_convId, "A", "agent", "Q2", {"a", "b"}, "single", 0);
        QVERIFY(!pidB.isEmpty());
        auto messagesAfter = m_msgs->getMessages(m_convId);
        QCOMPARE(messagesAfter.size(), 1);
        const Message& m = messagesAfter.first();
        QCOMPARE(m.role, QStringLiteral("system"));
        QVERIFY2(m.content.contains("Poll started: Q2"),
                 qPrintable(QStringLiteral("expected 'Poll started: Q2'; got: %1").arg(m.content)));
        QCOMPARE(m.metadata.value("poll_id").toString(), pidB);
        QCOMPARE(m.metadata.value("produced_by").toString(), QStringLiteral("poll_service"));
        QCOMPARE(m.metadata.value("poll_question").toString(), QStringLiteral("Q2"));
    }

    void test_PollsForConversation_OpenOnlyFilter() {
        const QString p1 = m_svc->createPoll(m_convId, "A", "agent", "Q1", {"a", "b"}, "single", 0);
        const QString p2 = m_svc->createPoll(m_convId, "A", "agent", "Q2", {"a", "b"}, "single", 0);
        QVERIFY(m_svc->closePoll(p2));

        const auto all = m_svc->pollsForConversation(m_convId, false);
        const auto open = m_svc->pollsForConversation(m_convId, true);
        QCOMPARE(all.size(), 2);
        QCOMPARE(open.size(), 1);
        QCOMPARE(open.first().toMap().value("id").toString(), p1);
    }

  private:
    QTemporaryDir m_dbDir;
    QString m_dbPath;
    QString m_convId;
    std::unique_ptr<ConversationService> m_convs;
    std::unique_ptr<MessageService> m_msgs;
    std::unique_ptr<PollService> m_svc;
};

QTEST_MAIN(TestPollService)
#include "test-poll-service.moc"
