// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "services/chat/inline-thinking-hoister.h"

#include <QTest>

#include <QString>

using Chat::InlineThinkingHoister;

class TestInlineThinkingHoister : public QObject {
    Q_OBJECT

  private slots:


    void test_emptyDelta_isNoop() {
        InlineThinkingHoister h;
        const auto r = h.feed(QString());
        QVERIFY(r.content.isEmpty());
        QVERIFY(r.thinking.isEmpty());
        QVERIFY(!h.isInsideThink());
    }

    void test_plainText_passesThrough() {
        InlineThinkingHoister h;
        const auto r = h.feed(QStringLiteral("hello, world"));
        QCOMPARE(r.content, QStringLiteral("hello, world"));
        QVERIFY(r.thinking.isEmpty());
    }


    void test_completeBlockInOneFeed() {
        InlineThinkingHoister h;
        const auto r = h.feed(QStringLiteral("hello <think>internal reasoning</think> world"));
        QCOMPARE(r.content, QStringLiteral("hello  world"));
        QCOMPARE(r.thinking, QStringLiteral("internal reasoning"));
        QVERIFY(!h.isInsideThink());
    }

    void test_completeEmptyBlock_passesAsEmptyThinking() {
        InlineThinkingHoister h;
        const auto r = h.feed(QStringLiteral("a<think></think>b"));
        QCOMPARE(r.content, QStringLiteral("ab"));
        QVERIFY(r.thinking.isEmpty());
        QVERIFY(!h.isInsideThink());
    }

    void test_multipleBlocksInOneFeed() {
        InlineThinkingHoister h;
        const auto r = h.feed(QStringLiteral("<think>one</think>between<think>two</think>end"));
        QCOMPARE(r.content, QStringLiteral("betweenend"));
        QCOMPARE(r.thinking, QStringLiteral("onetwo"));
        QVERIFY(!h.isInsideThink());
    }


    void test_openerCloserSplitAcrossFeeds() {
        InlineThinkingHoister h;
        const auto r1 = h.feed(QStringLiteral("abc <think>fo"));
        QCOMPARE(r1.content, QStringLiteral("abc "));
        QCOMPARE(r1.thinking, QStringLiteral("fo"));
        QVERIFY(h.isInsideThink());

        const auto r2 = h.feed(QStringLiteral("o</think> def"));
        QCOMPARE(r2.content, QStringLiteral(" def"));
        QCOMPARE(r2.thinking, QStringLiteral("o"));
        QVERIFY(!h.isInsideThink());
    }


    void test_partialOpenerAtTail_heldUntilNextFeed() {
        InlineThinkingHoister h;
        const auto r1 = h.feed(QStringLiteral("hello <thi"));
        QCOMPARE(r1.content, QStringLiteral("hello "));
        QVERIFY(r1.thinking.isEmpty());
        QVERIFY(!h.isInsideThink());

        const auto r2 = h.feed(QStringLiteral("nk>bar</think> done"));
        QCOMPARE(r2.content, QStringLiteral(" done"));
        QCOMPARE(r2.thinking, QStringLiteral("bar"));
        QVERIFY(!h.isInsideThink());
    }

    void test_partialCloserAtTail_heldUntilNextFeed() {
        InlineThinkingHoister h;
        const auto r1 = h.feed(QStringLiteral("<think>some thinking</thi"));
        QVERIFY(r1.content.isEmpty());
        QCOMPARE(r1.thinking, QStringLiteral("some thinking"));
        QVERIFY(h.isInsideThink());

        const auto r2 = h.feed(QStringLiteral("nk>visible tail"));
        QCOMPARE(r2.content, QStringLiteral("visible tail"));
        QVERIFY(r2.thinking.isEmpty());
        QVERIFY(!h.isInsideThink());
    }

    void test_partialOpenerNeverCompletes_stillHeld() {
        InlineThinkingHoister h;
        const auto r1 = h.feed(QStringLiteral("hi <thi"));
        QCOMPARE(r1.content, QStringLiteral("hi "));

        const auto r2 = h.feed(QStringLiteral("ng go"));
        QCOMPARE(r2.content, QStringLiteral("<thing go"));
        QVERIFY(r2.thinking.isEmpty());
        QVERIFY(!h.isInsideThink());
    }


    void test_reset_clearsCarryAndInsideFlag() {
        InlineThinkingHoister h;
        h.feed(QStringLiteral("<think>partial"));
        QVERIFY(h.isInsideThink());

        h.reset();
        QVERIFY(!h.isInsideThink());

        const auto r = h.feed(QStringLiteral("plain text"));
        QCOMPARE(r.content, QStringLiteral("plain text"));
        QVERIFY(r.thinking.isEmpty());
    }


    void test_tokenByTokenStreaming() {
        InlineThinkingHoister h;
        const QStringList tokens = {
            QStringLiteral("Hello"),
            QStringLiteral(" "),
            QStringLiteral("<"),
            QStringLiteral("think"),
            QStringLiteral(">"),
            QStringLiteral("re"),
            QStringLiteral("ason"),
            QStringLiteral("</"),
            QStringLiteral("think>"),
            QStringLiteral(" answer"),
        };
        QString content;
        QString thinking;
        for (const QString& t : tokens) {
            const auto r = h.feed(t);
            content += r.content;
            thinking += r.thinking;
        }
        QCOMPARE(content, QStringLiteral("Hello  answer"));
        QCOMPARE(thinking, QStringLiteral("reason"));
        QVERIFY(!h.isInsideThink());
    }


    void test_caseInsensitiveTags() {
        InlineThinkingHoister h;
        const auto r = h.feed(QStringLiteral("a<Think>UPPER</THINK>b"));
        QCOMPARE(r.content, QStringLiteral("ab"));
        QCOMPARE(r.thinking, QStringLiteral("UPPER"));
    }
};

QTEST_MAIN(TestInlineThinkingHoister)
#include "test-inline-thinking-hoister.moc"
