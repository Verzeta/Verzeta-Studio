// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "services/chat/action-intent-confirmer.h"

#include <QTest>

using Chat::ActionIntentConfirmer;
using Result = Chat::ActionIntentConfirmer::Result;

class TestActionIntentConfirmer : public QObject {
    Q_OBJECT

  private slots:
    void parse_yes() {
        QCOMPARE(ActionIntentConfirmer::parseIntentAnswer(QStringLiteral("YES")),
                 Result::Confirmed);
        QCOMPARE(ActionIntentConfirmer::parseIntentAnswer(QStringLiteral(" yes ")),
                 Result::Confirmed);
        QCOMPARE(ActionIntentConfirmer::parseIntentAnswer(QStringLiteral("Yes.")),
                 Result::Confirmed);
    }
    void parse_no() {
        QCOMPARE(ActionIntentConfirmer::parseIntentAnswer(QStringLiteral("NO")), Result::Rejected);
        QCOMPARE(ActionIntentConfirmer::parseIntentAnswer(QStringLiteral("no, this is final")),
                 Result::Rejected);
    }
    void parse_wrapped() {
        QCOMPARE(ActionIntentConfirmer::parseIntentAnswer(QStringLiteral("The answer is YES.")),
                 Result::Confirmed);
        QCOMPARE(ActionIntentConfirmer::parseIntentAnswer(QStringLiteral("Answer: NO")),
                 Result::Rejected);
    }
    void parse_empty_or_unparseable_isUnavailable() {
        QCOMPARE(ActionIntentConfirmer::parseIntentAnswer(QString()), Result::Unavailable);
        QCOMPARE(ActionIntentConfirmer::parseIntentAnswer(QStringLiteral("   ")),
                 Result::Unavailable);
        QCOMPARE(ActionIntentConfirmer::parseIntentAnswer(QStringLiteral("maybe, hard to say")),
                 Result::Unavailable);
    }

    void prompt_containsTurnAndContract() {
        const QString p = ActionIntentConfirmer::buildIntentPrompt(
            QStringLiteral("I will first create a basic styles.css."));
        QVERIFY(p.contains(QStringLiteral("styles.css")));
        QVERIFY(p.contains(QStringLiteral("YES")));
        QVERIFY(p.contains(QStringLiteral("NO")));
    }

    void prompt_languageAgnosticAndSummaryNoCases() {
        const QString p = ActionIntentConfirmer::buildIntentPrompt(QStringLiteral("Anything."));
        QVERIFY(p.contains(QStringLiteral("ANY language")));
        QVERIFY(p.contains(QStringLiteral("summarises")));
    }

    void prompt_conservativeDefaultAndSummaryCases() {
        const QString p = ActionIntentConfirmer::buildIntentPrompt(
            QStringLiteral("I have finished editing report.md. Can we close?"));
        QVERIFY(p.contains(QStringLiteral("when in ANY doubt, answer NO")));
        QVERIFY(p.contains(QStringLiteral("WAITING for the user")));
        QVERIFY(p.contains(QStringLiteral("ALREADY DONE")));
        QVERIFY(p.contains(QStringLiteral("YES ONLY")));
    }
    void prompt_trimsLongBodies() {
        QString longBody = QString(QStringLiteral("filler ")).repeated(600);
        longBody += QStringLiteral(" Finally, I'll create pricing.md.");
        const QString p = ActionIntentConfirmer::buildIntentPrompt(longBody);
        QVERIFY(p.contains(QStringLiteral("[middle elided]")));
        QVERIFY(p.contains(QStringLiteral("pricing.md")));
    }

    void noProvider_isUnavailable() {
        ActionIntentConfirmer c(nullptr);
        QVERIFY(!c.canConfirm());
        auto f = c.confirmFileCreationIntentAsync(QStringLiteral("I'll write x.py"));
        QVERIFY(f.isFinished());
        QCOMPARE(f.result(), Result::Unavailable);
    }
    void testDecider_overridesResult() {
        ActionIntentConfirmer c(nullptr);
        c.setTestDecider([](const QString&) { return Result::Confirmed; });
        QVERIFY(c.canConfirm());
        auto f = c.confirmFileCreationIntentAsync(QStringLiteral("anything"));
        QVERIFY(f.isFinished());
        QCOMPARE(f.result(), Result::Confirmed);

        c.setTestDecider([](const QString&) { return Result::Rejected; });
        auto g = c.confirmFileCreationIntentAsync(QStringLiteral("anything"));
        QVERIFY(g.isFinished());
        QCOMPARE(g.result(), Result::Rejected);
    }
};

QTEST_MAIN(TestActionIntentConfirmer)
#include "test-action-intent-confirmer.moc"
