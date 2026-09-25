// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "services/ragp/ragp-rule-classifier.h"
#include "services/ragp/ragp-types.h"

#include <QTest>

using Ragp::Classification;
using Ragp::Intent;
using Ragp::Request;
using Ragp::RuleClassifier;
using Ragp::Target;

class TestRagpRuleClassifier : public QObject {
    Q_OBJECT

  private:
    static Request
    makeReq(const QString& content, const QString& author, const QStringList& roster) {
        Request r;
        r.content = content;
        r.authorAlias = author;
        r.rosterAliases = roster;
        return r;
    }

  private slots:

    void test_noMentions_emptyResult() {
        const Classification c =
            RuleClassifier::classify(makeReq(QStringLiteral("Just a plain message."),
                                             QStringLiteral("Alice"),
                                             {QStringLiteral("Alice"), QStringLiteral("Bob")}));
        QVERIFY(c.targets.isEmpty());
        QVERIFY(!c.routeToUser);
        QCOMPARE(c.confidence, 1.0);
        QCOMPARE(c.source, QStringLiteral("rule"));
    }

    void test_selfMention_dropped() {
        const Classification c =
            RuleClassifier::classify(makeReq(QStringLiteral("I'm @Alice and I'm here."),
                                             QStringLiteral("Alice"),
                                             {QStringLiteral("Alice"), QStringLiteral("Bob")}));
        QVERIFY(c.targets.isEmpty());
        QVERIFY(!c.routeToUser);
        QCOMPARE(c.confidence, 1.0);
    }

    void test_userAliases_escalateToUser() {
        const Classification c =
            RuleClassifier::classify(makeReq(QStringLiteral("Hey @owner, I need help."),
                                             QStringLiteral("Alice"),
                                             {QStringLiteral("Alice")}));
        QVERIFY(c.targets.isEmpty());
        QVERIFY(c.routeToUser);
        QCOMPARE(c.confidence, 1.0);
    }

    void test_userAlias_caseInsensitive() {
        const Classification c = RuleClassifier::classify(makeReq(
            QStringLiteral("@OWNER please check"), QStringLiteral("Bob"), {QStringLiteral("Bob")}));
        QVERIFY(c.routeToUser);
    }

    void test_allUserAliasKeywords() {
        const QStringList keywords = {
            QStringLiteral("owner"),
            QStringLiteral("user"),
            QStringLiteral("you"),
            QStringLiteral("leader"),
        };
        for (const QString& kw : keywords) {
            const QString msg = QStringLiteral("Ping @%1").arg(kw);
            const Classification c =
                RuleClassifier::classify(makeReq(msg, QStringLiteral("A"), {QStringLiteral("A")}));
            QVERIFY2(c.routeToUser, qPrintable(kw));
        }
    }

    void test_broadcast_populatesAllRosterMembers() {
        const Classification c = RuleClassifier::classify(
            makeReq(QStringLiteral("@everyone please intro yourselves"),
                    QStringLiteral("Alice"),
                    {QStringLiteral("Alice"), QStringLiteral("Bob"), QStringLiteral("Carol")}));
        QCOMPARE(c.targets.size(), 2);
        for (const Target& t : c.targets) {
            QCOMPARE(t.intent, Intent::BROADCAST_REQUEST);
            QVERIFY(t.alias != QStringLiteral("Alice"));
        }
        QCOMPARE(c.confidence, 1.0);
    }

    void test_broadcastAliases_all_everyone_team() {
        const QStringList broadcasts = {
            QStringLiteral("all"),
            QStringLiteral("everyone"),
            QStringLiteral("team"),
        };
        for (const QString& bc : broadcasts) {
            const Classification c =
                RuleClassifier::classify(makeReq(QStringLiteral("@%1 hi").arg(bc),
                                                 QStringLiteral("A"),
                                                 {QStringLiteral("A"), QStringLiteral("B")}));
            QCOMPARE(c.targets.size(), 1);
            QCOMPARE(c.targets[0].intent, Intent::BROADCAST_REQUEST);
        }
    }

    void test_codeFenceMention_classifiedAsReference() {
        const Classification c = RuleClassifier::classify(
            makeReq(QStringLiteral("Here's my code:\n```\n@Bob wrote this\n```"),
                    QStringLiteral("Alice"),
                    {QStringLiteral("Alice"), QStringLiteral("Bob")}));
        QCOMPARE(c.targets.size(), 1);
        QCOMPARE(c.targets[0].alias, QStringLiteral("Bob"));
        QCOMPARE(c.targets[0].intent, Intent::REFERENCE);
        QCOMPARE(c.confidence, 1.0);
    }

    void test_blockquoteMention_classifiedAsQuoting() {
        const Classification c =
            RuleClassifier::classify(makeReq(QStringLiteral("Prior note:\n> @Bob said earlier"),
                                             QStringLiteral("Alice"),
                                             {QStringLiteral("Alice"), QStringLiteral("Bob")}));
        QCOMPARE(c.targets.size(), 1);
        QCOMPARE(c.targets[0].intent, Intent::QUOTING);
        QCOMPARE(c.confidence, 1.0);
    }

    void test_unknownAlias_dropped() {
        const Classification c =
            RuleClassifier::classify(makeReq(QStringLiteral("Hey @Zephyr, can you help?"),
                                             QStringLiteral("Alice"),
                                             {QStringLiteral("Alice"), QStringLiteral("Bob")}));
        QVERIFY(c.targets.isEmpty());
        QVERIFY(!c.routeToUser);
    }

    void test_ambiguousMention_flaggedUnknown() {
        const Classification c =
            RuleClassifier::classify(makeReq(QStringLiteral("Hey @Bob, quick question"),
                                             QStringLiteral("Alice"),
                                             {QStringLiteral("Alice"), QStringLiteral("Bob")}));
        QCOMPARE(c.targets.size(), 1);
        QCOMPARE(c.targets[0].intent, Intent::UNKNOWN);
        QVERIFY(c.confidence < 1.0);
    }

    void test_mixedUserAndBroadcast() {
        const Classification c =
            RuleClassifier::classify(makeReq(QStringLiteral("@owner FYI, @everyone please review"),
                                             QStringLiteral("Alice"),
                                             {QStringLiteral("Alice"), QStringLiteral("Bob")}));
        QVERIFY(c.routeToUser);
        QCOMPARE(c.targets.size(), 1);
        QCOMPARE(c.targets[0].intent, Intent::BROADCAST_REQUEST);
    }

    void test_duplicateMentions_deduplicated() {
        const Classification c =
            RuleClassifier::classify(makeReq(QStringLiteral("@Bob and @Bob and also @Bob"),
                                             QStringLiteral("Alice"),
                                             {QStringLiteral("Alice"), QStringLiteral("Bob")}));
        QCOMPARE(c.targets.size(), 1);
    }

    void test_caseInsensitiveRosterMatch() {
        const Classification c =
            RuleClassifier::classify(makeReq(QStringLiteral("@BOB help"),
                                             QStringLiteral("Alice"),
                                             {QStringLiteral("Alice"), QStringLiteral("Bob")}));
        QCOMPARE(c.targets.size(), 1);
        QCOMPARE(c.targets[0].intent, Intent::UNKNOWN);
    }

    void test_spaceAliasNormalization() {
        const Classification c =
            RuleClassifier::classify(makeReq(QStringLiteral("@PM_Alice can you help?"),
                                             QStringLiteral("Bob"),
                                             {QStringLiteral("PM Alice"), QStringLiteral("Bob")}));
        QCOMPARE(c.targets.size(), 1);
    }

    void test_emptyContent() {
        const Classification c = RuleClassifier::classify(
            makeReq(QString(), QStringLiteral("Alice"), {QStringLiteral("Alice")}));
        QVERIFY(c.targets.isEmpty());
        QCOMPARE(c.confidence, 1.0);
    }

    void test_multipleRosterMentions() {
        const Classification c = RuleClassifier::classify(
            makeReq(QStringLiteral("@Bob and @Carol, can you collaborate?"),
                    QStringLiteral("Alice"),
                    {QStringLiteral("Alice"), QStringLiteral("Bob"), QStringLiteral("Carol")}));
        QCOMPARE(c.targets.size(), 2);
        QVERIFY(c.confidence < 1.0);
    }

    void test_intentStringRoundtrip() {
        const QList<Intent> all = {
            Intent::UNKNOWN,
            Intent::DELEGATE_RESPONSE,
            Intent::DELEGATE_TASK,
            Intent::BROADCAST_REQUEST,
            Intent::REFERENCE,
            Intent::ACKNOWLEDGMENT,
            Intent::ESCALATION_TO_USER,
        };
        for (Intent i : all) {
            const QString s = Ragp::intentToString(i);
            QVERIFY2(!s.isEmpty(), qPrintable(s));
            QCOMPARE(Ragp::intentFromString(s), i);
        }
    }

    void test_shouldCascadePolicy() {
        QVERIFY(Ragp::shouldCascade(Intent::DELEGATE_RESPONSE));
        QVERIFY(Ragp::shouldCascade(Intent::DELEGATE_TASK));
        QVERIFY(Ragp::shouldCascade(Intent::BROADCAST_REQUEST));
        QVERIFY(Ragp::shouldCascade(Intent::CLARIFICATION_REQUEST));
        QVERIFY(Ragp::shouldCascade(Intent::CORRECTION_REQUEST));
        QVERIFY(Ragp::shouldCascade(Intent::STATUS_CHECK));
        QVERIFY(Ragp::shouldCascade(Intent::HANDOFF_COMPLETE));
        QVERIFY(Ragp::shouldCascade(Intent::PLURAL_ADDRESS));

        QVERIFY(!Ragp::shouldCascade(Intent::UNKNOWN));
        QVERIFY(!Ragp::shouldCascade(Intent::REFERENCE));
        QVERIFY(!Ragp::shouldCascade(Intent::ACKNOWLEDGMENT));
        QVERIFY(!Ragp::shouldCascade(Intent::SUMMARY_LIST));
        QVERIFY(!Ragp::shouldCascade(Intent::QUESTION_ABOUT));
        QVERIFY(!Ragp::shouldCascade(Intent::QUOTING));
        QVERIFY(!Ragp::shouldCascade(Intent::GREETING_FAREWELL));
        QVERIFY(!Ragp::shouldCascade(Intent::ESCALATION_TO_USER));
    }
};

QTEST_MAIN(TestRagpRuleClassifier)
#include "test-ragp-rule-classifier.moc"
