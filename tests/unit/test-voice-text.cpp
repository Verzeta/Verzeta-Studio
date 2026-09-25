// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: GPL-3.0-or-later


#include "voice/voice-text.h"

#include <QTest>

#include <QObject>

using Verzeta::Voice::splitIntoSentences;
using Verzeta::Voice::toSpeakableText;

class TestVoiceText : public QObject {
    Q_OBJECT

  private slots:
    void test_plainTextIsUnchanged() {
        QCOMPARE(toSpeakableText(QStringLiteral("Hello there.")), QStringLiteral("Hello there."));
    }

    void test_emptyInputSaysNothing() {
        QVERIFY(toSpeakableText(QString()).isEmpty());
        QVERIFY(toSpeakableText(QStringLiteral("   \n  ")).isEmpty());
    }

    void test_fencedCodeBecomesOneCue() {
        const QString md = QStringLiteral("Here it is:\n```cpp\nint main() { return 0; }\n"
                                          "// a long comment\n```\nDone.");
        const QString out = toSpeakableText(md);
        QVERIFY(out.contains(QStringLiteral("Code omitted.")));
        QVERIFY(!out.contains(QStringLiteral("int main")));
        QVERIFY(out.startsWith(QStringLiteral("Here it is:")));
        QVERIFY(out.endsWith(QStringLiteral("Done.")));
    }

    void test_inlineCodeKeepsItsWords() {
        QCOMPARE(toSpeakableText(QStringLiteral("Run `git status` now.")),
                 QStringLiteral("Run git status now."));
    }

    void test_headingsBulletsAndEmphasisAreUnwrapped() {
        const QString md = QStringLiteral("## Plan\n\n- **First** item\n- _Second_ item\n");
        QCOMPARE(toSpeakableText(md), QStringLiteral("Plan First item Second item"));
    }

    void test_linkKeepsLabelDropsUrl() {
        QCOMPARE(toSpeakableText(QStringLiteral("See [the docs](https://x.test/a_b).")),
                 QStringLiteral("See the docs."));
    }

    void test_mentionLosesTheAtSign() {
        QCOMPARE(toSpeakableText(QStringLiteral("@Kate please draft it.")),
                 QStringLiteral("Kate please draft it."));
    }

    void test_emojiAreDropped() {
        const QString out = toSpeakableText(QString::fromUtf8("Shipped \xE2\x9C\x85 today."));
        QCOMPARE(out, QStringLiteral("Shipped today."));
    }

    void test_whitespaceIsCollapsed() {
        QCOMPARE(toSpeakableText(QStringLiteral("One.\n\n\nTwo.")), QStringLiteral("One. Two."));
    }


    void test_splitEmpty() { QVERIFY(splitIntoSentences(QString()).isEmpty()); }

    void test_splitSingleSentence() {
        const QStringList out = splitIntoSentences(QStringLiteral("Just the one sentence here."));
        QCOMPARE(out.size(), 1);
        QCOMPARE(out.first(), QStringLiteral("Just the one sentence here."));
    }

    void test_splitsOnSentenceBoundaries() {
        const QStringList out =
            splitIntoSentences(QStringLiteral("The launch page is ready for review. "
                                              "I also fixed the hero spacing. "
                                              "Tell me if the copy needs work."));
        QCOMPARE(out.size(), 3);
        QVERIFY(out.at(0).endsWith(QStringLiteral("review.")));
        QVERIFY(out.at(1).startsWith(QStringLiteral("I also")));
    }

    void test_decimalDoesNotSplit() {
        const QStringList out =
            splitIntoSentences(QStringLiteral("The value is 3.5 and that is the final answer."));
        QCOMPARE(out.size(), 1);
    }

    void test_shortFragmentsMerge() {
        const QStringList out = splitIntoSentences(
            QStringLiteral("Yes. The rest of this sentence is long enough to stand alone."));
        QCOMPARE(out.size(), 1);
        QVERIFY(out.first().startsWith(QStringLiteral("Yes.")));
    }


    void test_pipeTableBecomesOneCue() {
        const QString out = toSpeakableText(QStringLiteral("Here is a quick comparison.\n\n"
                                                           "| Feature | Verzeta | Others |\n"
                                                           "|---------|---------|--------|\n"
                                                           "| Local   | Yes     | No     |\n"
                                                           "| Price   | Free    | $20/mo |\n\n"
                                                           "So Verzeta wins on both."));
        QCOMPARE(out,
                 QStringLiteral("Here is a quick comparison. A table of Feature, Verzeta and "
                                "Others with 2 rows is on screen. So Verzeta wins on both."));
    }

    void test_borderlessTableBecomesOneCue() {
        const QString out = toSpeakableText(QStringLiteral("Results below.\n\n"
                                                           "Feature | Verzeta | Others\n"
                                                           "------- | ------- | ------\n"
                                                           "Local   | Yes     | No\n\n"
                                                           "That is all."));
        QCOMPARE(out,
                 QStringLiteral("Results below. A table of Feature, Verzeta and Others with "
                                "one row is on screen. That is all."));
    }

    void test_htmlTableBecomesOneCue() {
        const QString out = toSpeakableText(
            QStringLiteral("Look here. <table><tr><td>a</td><td>b</td></tr></table> Done."));
        QCOMPARE(out,
                 QStringLiteral("Look here. A table is on screen. "
                                "Done."));
    }

    void test_otherHtmlTagsAreDroppedButTextIsKept() {
        QCOMPARE(toSpeakableText(QStringLiteral("A <b>bold</b> claim.")),
                 QStringLiteral("A bold claim."));
    }

    void test_proseContainingAPipeIsNotATable() {
        QCOMPARE(toSpeakableText(QStringLiteral("Use grep foo | wc -l to count them.")),
                 QStringLiteral("Use grep foo | wc -l to count them."));
        QCOMPARE(toSpeakableText(QStringLiteral("The options are red | green | blue.")),
                 QStringLiteral("The options are red | green | blue."));
    }

    void test_tableInsideCodeFenceStaysACodeCue() {
        const QString out = toSpeakableText(
            QStringLiteral("Example:\n\n```\n| a | b |\n|---|---|\n| 1 | 2 |\n```\n"));
        QCOMPARE(out, QStringLiteral("Example: Code omitted."));
    }

    void test_tableOnlyReplyStillSpeaksTheCue() {
        const QString out = toSpeakableText(QStringLiteral("| a | b |\n|---|---|\n| 1 | 2 |\n"));
        QCOMPARE(out, QStringLiteral("A table of a and b with one row is on screen."));
        QCOMPARE(splitIntoSentences(out).size(), 1);
    }

    void test_nestedBulletsAreSpokenNotCodeOmitted() {
        const QString out = toSpeakableText(
            QStringLiteral("- Strengths:\n"
                           "    - Compatibility: Unmatched compatibility with legacy "
                           "software.\n"
                           "    - User Experience: The interface is highly polished.\n"));
        QVERIFY(!out.contains(QStringLiteral("Code omitted")));
        QVERIFY(out.contains(QStringLiteral("Unmatched compatibility")));
        QVERIFY(out.contains(QStringLiteral("highly polished")));
    }

    void test_numberedSubItemsAreSpoken() {
        const QString out = toSpeakableText(QStringLiteral("1. First:\n"
                                                           "    1. Alpha detail here.\n"
                                                           "    2. Beta detail here.\n"
                                                           "        - Even deeper gamma.\n"));
        QVERIFY(!out.contains(QStringLiteral("Code omitted")));
        QVERIFY(out.contains(QStringLiteral("Beta detail")));
        QVERIFY(out.contains(QStringLiteral("gamma")));
    }

    void test_indentedTableIsSummarisedNotCodeOmitted() {
        const QString out = toSpeakableText(QStringLiteral("- Comparison:\n\n"
                                                           "    | Feature | Verzeta | Others |\n"
                                                           "    |---------|---------|--------|\n"
                                                           "    | Local   | Yes     | No     |\n"));
        QVERIFY(!out.contains(QStringLiteral("Code omitted")));
        QVERIFY2(out.contains(QStringLiteral("A table of Feature, Verzeta and Others")),
                 qPrintable(out));
    }

    void test_trueIndentedCodeIsStillCued() {
        const QString out = toSpeakableText(QStringLiteral("Run this:\n\n"
                                                           "    for x in range(3):\n"
                                                           "        print(x)\n\n"
                                                           "Then continue."));
        QCOMPARE(out, QStringLiteral("Run this: Code omitted. Then continue."));
    }

    void test_splitLosesNothing() {
        const QString text =
            QStringLiteral("First sentence is right here. Second sentence follows it. "
                           "Third one closes.");
        QCOMPARE(splitIntoSentences(text).join(QLatin1Char(' ')), text);
    }
};

QTEST_MAIN(TestVoiceText)
#include "test-voice-text.moc"
