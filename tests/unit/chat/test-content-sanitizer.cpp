// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "services/chat/content-sanitizer.h"

#include <QTest>

#include <memory>

using Chat::ContentSanitizer;
using Chat::SanitizeInputs;
using Chat::SanitizeResult;

class TestContentSanitizer : public QObject {
    Q_OBJECT

  private:
    std::unique_ptr<ContentSanitizer> m_sanitizer;

    static SanitizeInputs makeInputs(const QString& content,
                                     const QString& responderAlias = {},
                                     bool isGroupChat = false,
                                     const QStringList& roster = {},
                                     bool activeTaskPlanSet = false) {
        SanitizeInputs in;
        in.content = content;
        in.responderAlias = responderAlias;
        in.isGroupChat = isGroupChat;
        in.rosterAliases = roster;
        in.activeTaskPlanSet = activeTaskPlanSet;
        return in;
    }

  private slots:
    void init() { m_sanitizer = std::make_unique<ContentSanitizer>(); }

    void cleanup() { m_sanitizer.reset(); }

    void test_emptyInput_returnsEmptyResult() {
        const auto r = m_sanitizer->sanitize(makeInputs({}));
        QVERIFY(r.sanitisedContent.isEmpty());
        QVERIFY(r.declaredTaskStatus.isEmpty());
        QCOMPARE(r.charsStripped, 0);
    }

    void test_harmonyChannelMarkers_stripped() {
        auto clean = [&](const QString& in) {
            return m_sanitizer->sanitize(makeInputs(in)).sanitisedContent;
        };
        QCOMPARE(clean(QStringLiteral("Next up is the website.<channel|>")),
                 QStringLiteral("Next up is the website."));
        QCOMPARE(clean(QStringLiteral("Plan ready.<|channel|>")), QStringLiteral("Plan ready."));
        QCOMPARE(clean(QStringLiteral("Reply.<|channel|>final")), QStringLiteral("Reply."));
        QCOMPARE(clean(QStringLiteral("Done.<|message|>")), QStringLiteral("Done."));
        QCOMPARE(clean(QStringLiteral("loop until <end> of file")),
                 QStringLiteral("loop until <end> of file"));
    }

    void test_plainContent_unchanged() {
        const QString input = QStringLiteral("A clean sentence with no attribution tags.");
        const auto r = m_sanitizer->sanitize(makeInputs(input));
        QCOMPARE(r.sanitisedContent, input);
        QVERIFY(r.declaredTaskStatus.isEmpty());
    }

    void test_leadingNudge_stripped() {
        const QString input = QStringLiteral("[@Sam, it is your turn to respond.]\n"
                                             "Here is the real reply.");
        const auto r = m_sanitizer->sanitize(makeInputs(input));
        QCOMPARE(r.sanitisedContent, QStringLiteral("Here is the real reply."));
    }

    void test_selfLARP_strippedInGroupChat() {
        const QString input = QStringLiteral("(Alice said) my actual content here.");
        const auto r =
            m_sanitizer->sanitize(makeInputs(input,
                                             QStringLiteral("Alice"),
                                             true,
                                             {QStringLiteral("Alice"), QStringLiteral("Bob")}));
        QCOMPARE(r.sanitisedContent, QStringLiteral("my actual content here."));
    }

    void test_crossLARP_strippedFromRoster() {
        const QString input = QStringLiteral("(Bob said) my reply attributed to Bob.");
        const auto r =
            m_sanitizer->sanitize(makeInputs(input,
                                             QStringLiteral("Alice"),
                                             true,
                                             {QStringLiteral("Alice"), QStringLiteral("Bob")}));
        QCOMPARE(r.sanitisedContent, QStringLiteral("my reply attributed to Bob."));
        QCOMPARE(r.impersonatedAlias, QStringLiteral("Bob"));
    }

    void test_stackedLARP_strippedInSinglePass() {
        const QString input = QStringLiteral("(Alice said)(Bob said) actual body.");
        const auto r =
            m_sanitizer->sanitize(makeInputs(input,
                                             QStringLiteral("Alice"),
                                             true,
                                             {QStringLiteral("Alice"), QStringLiteral("Bob")}));
        QCOMPARE(r.sanitisedContent, QStringLiteral("actual body."));
        QCOMPARE(r.impersonatedAlias, QStringLiteral("Bob"));
    }

    void test_impersonation_selfPrefix_notFlagged() {
        const auto r =
            m_sanitizer->sanitize(makeInputs(QStringLiteral("(Alice said) my own voice."),
                                             QStringLiteral("Alice"),
                                             true,
                                             {QStringLiteral("Alice"), QStringLiteral("Bob")}));
        QVERIFY(r.impersonatedAlias.isEmpty());
    }

    void test_impersonation_userPseudonym_notFlagged() {
        const auto r = m_sanitizer->sanitize(
            makeInputs(QStringLiteral("(Human said) something the user asked."),
                       QStringLiteral("Alice"),
                       true,
                       {QStringLiteral("Alice"), QStringLiteral("Bob")}));
        QVERIFY(r.impersonatedAlias.isEmpty());
    }

    void test_impersonation_never1to1() {
        const auto r = m_sanitizer->sanitize(makeInputs(
            QStringLiteral("(Bob said) some content."), QStringLiteral("Alice"), false, {}));
        QVERIFY(r.impersonatedAlias.isEmpty());
    }

    void test_impersonation_underscoreVariantFlagged() {
        const auto r = m_sanitizer->sanitize(
            makeInputs(QStringLiteral("(Product_Manager said) imposter body."),
                       QStringLiteral("Alice"),
                       true,
                       {QStringLiteral("Alice"), QStringLiteral("Product Manager")}));
        QCOMPARE(r.impersonatedAlias, QStringLiteral("Product Manager"));
        QCOMPARE(r.sanitisedContent, QStringLiteral("imposter body."));
    }

    void test_userPseudonymPrefix_strippedAlways() {
        const QString input = QStringLiteral("(Human said) fake-quoting the human.");
        const auto r = m_sanitizer->sanitize(makeInputs(input));
        QCOMPARE(r.sanitisedContent, QStringLiteral("fake-quoting the human."));
    }

    void test_midContentTranscriptEcho_truncatedAtOtherAlias() {
        const QString input = QStringLiteral("my color is blue.\n\n"
                                             "(Bob said) fake transcript leaked here.");
        const auto r =
            m_sanitizer->sanitize(makeInputs(input,
                                             QStringLiteral("Alice"),
                                             true,
                                             {QStringLiteral("Alice"), QStringLiteral("Bob")}));
        QCOMPARE(r.sanitisedContent, QStringLiteral("my color is blue."));
    }

    void test_midContentSelfAlias_notTruncated() {
        const QString input = QStringLiteral("header line.\n\n(Alice said) my own self-reference.");
        const auto r =
            m_sanitizer->sanitize(makeInputs(input,
                                             QStringLiteral("Alice"),
                                             true,
                                             {QStringLiteral("Alice"), QStringLiteral("Bob")}));
        QCOMPARE(r.sanitisedContent, input);
    }

    void test_selfMention_deSigiledInGroupChat() {
        const QString input =
            QStringLiteral("@Alice will handle pricing. As @alice I already noted this.");
        const auto r =
            m_sanitizer->sanitize(makeInputs(input,
                                             QStringLiteral("Alice"),
                                             true,
                                             {QStringLiteral("Alice"), QStringLiteral("Bob")}));
        QCOMPARE(r.sanitisedContent,
                 QStringLiteral("Alice will handle pricing. "
                                "As alice I already noted this."));
    }

    void test_selfMention_otherMembersAndBroadcastsPreserved() {
        const QString input =
            QStringLiteral("@Bob please review, @all take note - @Alice is on it, "
                           "and @owner should see the summary.");
        const auto r =
            m_sanitizer->sanitize(makeInputs(input,
                                             QStringLiteral("Alice"),
                                             true,
                                             {QStringLiteral("Alice"), QStringLiteral("Bob")}));
        QCOMPARE(r.sanitisedContent,
                 QStringLiteral("@Bob please review, @all take note - "
                                "Alice is on it, and @owner should see "
                                "the summary."));
    }

    void test_selfMention_underscoreNormalizedAliasVariant() {
        const QString input = QStringLiteral("@Project_Manager confirms the plan.");
        const auto r = m_sanitizer->sanitize(
            makeInputs(input,
                       QStringLiteral("Project Manager"),
                       true,
                       {QStringLiteral("Project Manager"), QStringLiteral("Bob")}));
        QCOMPARE(r.sanitisedContent, QStringLiteral("Project_Manager confirms the plan."));
    }

    void test_selfMention_emailAndLongerHandlesUntouched() {
        const QString input = QStringLiteral("Mail ops@Alice.example and ping @Alice2 about it.");
        const auto r =
            m_sanitizer->sanitize(makeInputs(input,
                                             QStringLiteral("Alice"),
                                             true,
                                             {QStringLiteral("Alice"), QStringLiteral("Alice2")}));
        QCOMPARE(r.sanitisedContent, input);
    }

    void test_selfMention_noopOutsideGroupChat() {
        const QString input = QStringLiteral("@Alice self-reference.");
        const auto r = m_sanitizer->sanitize(makeInputs(input, QStringLiteral("Alice"), false, {}));
        QCOMPARE(r.sanitisedContent, input);
    }

    void test_selfMention_idempotent() {
        const QString input = QStringLiteral("@Alice done; @Bob next.");
        const auto once =
            m_sanitizer->sanitize(makeInputs(input,
                                             QStringLiteral("Alice"),
                                             true,
                                             {QStringLiteral("Alice"), QStringLiteral("Bob")}));
        const auto twice =
            m_sanitizer->sanitize(makeInputs(once.sanitisedContent,
                                             QStringLiteral("Alice"),
                                             true,
                                             {QStringLiteral("Alice"), QStringLiteral("Bob")}));
        QCOMPARE(twice.sanitisedContent, once.sanitisedContent);
        QCOMPARE(once.sanitisedContent, QStringLiteral("Alice done; @Bob next."));
    }

    void test_thinkBlock_stripped() {
        const QString input = QStringLiteral("<think>internal reasoning</think>Final answer.");
        const auto r = m_sanitizer->sanitize(makeInputs(input));
        QCOMPARE(r.sanitisedContent, QStringLiteral("Final answer."));
    }

    void test_thinkBlock_multilineStripped() {
        const QString input = QStringLiteral("<think>\nstep one.\nstep two.\n</think>\n"
                                             "Answer after thinking.");
        const auto r = m_sanitizer->sanitize(makeInputs(input));
        QCOMPARE(r.sanitisedContent, QStringLiteral("Answer after thinking."));
    }


    void test_taskStateMarker_capturedAndStrippedWhenActive() {
        const QString input = QStringLiteral("Here is the completed work.\n"
                                             "<task_state>completed</task_state>");
        const auto r = m_sanitizer->sanitize(makeInputs(input, {}, false, {}, true));
        QCOMPARE(r.sanitisedContent, QStringLiteral("Here is the completed work."));
        QCOMPARE(r.declaredTaskStatus, QStringLiteral("completed"));
    }

    void test_taskStateMarker_strippedButClearedWhenInactive() {
        const QString input = QStringLiteral("Reply body.\n<task_state>completed</task_state>");
        const auto r = m_sanitizer->sanitize(makeInputs(input, {}, false, {}, false));
        QCOMPARE(r.sanitisedContent, QStringLiteral("Reply body."));
        QVERIFY2(r.declaredTaskStatus.isEmpty(),
                 "declaredTaskStatus must be empty when "
                 "activeTaskPlanSet is false");
    }

    void test_taskStateMarker_acceptsTaskStatusAndDashVariants() {
        const QString input = QStringLiteral("Dash variant reply.\n"
                                             "<task-status>Completed</task-status>");
        const auto r = m_sanitizer->sanitize(makeInputs(input, {}, false, {}, true));
        QCOMPARE(r.sanitisedContent, QStringLiteral("Dash variant reply."));
        QCOMPARE(r.declaredTaskStatus, QStringLiteral("completed"));
    }

    void test_combinedPipeline_allStagesInOneInput() {
        const QString input = QStringLiteral("[@Alice, it is your turn to respond.]\n"
                                             "(Alice said)(Bob said) "
                                             "<think>planning the reply</think>"
                                             "the real answer.\n"
                                             "<task_state>working</task_state>");
        const auto r =
            m_sanitizer->sanitize(makeInputs(input,
                                             QStringLiteral("Alice"),
                                             true,
                                             {QStringLiteral("Alice"), QStringLiteral("Bob")},
                                             true));
        QCOMPARE(r.sanitisedContent, QStringLiteral("the real answer."));
        QCOMPARE(r.declaredTaskStatus, QStringLiteral("working"));
    }

    void test_trailingSeparatorsChoppedAfterMidCutoff() {
        const QString input = QStringLiteral("my real answer.\n\n---\n\n(Bob said) leaked echo.");
        const auto r =
            m_sanitizer->sanitize(makeInputs(input,
                                             QStringLiteral("Alice"),
                                             true,
                                             {QStringLiteral("Alice"), QStringLiteral("Bob")}));
        QCOMPARE(r.sanitisedContent, QStringLiteral("my real answer."));
    }
};

QTEST_APPLESS_MAIN(TestContentSanitizer)
#include "test-content-sanitizer.moc"
