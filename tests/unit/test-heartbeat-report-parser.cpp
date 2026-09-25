// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/utils/heartbeat-report-parser.h"

#include <QTest>

#include <QString>

class TestHeartbeatReportParser : public QObject {
    Q_OBJECT

  private slots:
    void test_emptyInput_returnsInvalidEmpty() {
        const auto p = parseHeartbeatReport(QString());
        QVERIFY(!p.valid);
        QVERIFY(p.title.isEmpty());
        QVERIFY(p.body.isEmpty());
        QVERIFY(p.summary.isEmpty());
    }

    void test_threeLabels_sameLineContent() {
        const QString input = QStringLiteral("TITLE: Twitter trends today\n"
                                             "RESULTS: A spike around X.\n"
                                             "SUMMARY: New movement around X, +4× engagement.\n");
        const auto p = parseHeartbeatReport(input);
        QVERIFY(p.valid);
        QCOMPARE(p.title, QStringLiteral("Twitter trends today"));
        QCOMPARE(p.body, QStringLiteral("A spike around X."));
        QCOMPARE(p.summary, QStringLiteral("New movement around X, +4× engagement."));
    }

    void test_threeLabels_sectionsOnFollowingLines() {
        const QString input = QStringLiteral("TITLE: T\n"
                                             "RESULTS:\n"
                                             "Line one of the body.\n"
                                             "Line two of the body.\n"
                                             "SUMMARY:\n"
                                             "One short line.\n");
        const auto p = parseHeartbeatReport(input);
        QVERIFY(p.valid);
        QCOMPARE(p.title, QStringLiteral("T"));
        QVERIFY(p.body.startsWith(QStringLiteral("Line one")));
        QVERIFY(p.body.contains(QStringLiteral("Line two")));
        QCOMPARE(p.summary, QStringLiteral("One short line."));
    }

    void test_caseInsensitiveLabels() {
        const QString input = QStringLiteral("title: t\n"
                                             "results: r\n"
                                             "summary: s\n");
        const auto p = parseHeartbeatReport(input);
        QVERIFY(p.valid);
        QCOMPARE(p.title, QStringLiteral("t"));
        QCOMPARE(p.body, QStringLiteral("r"));
        QCOMPARE(p.summary, QStringLiteral("s"));
    }

    void test_titleTruncatedAt100Chars() {
        QString longTitle;
        for (int i = 0; i < 200; ++i)
            longTitle.append(QLatin1Char('a'));
        const QString input =
            QStringLiteral("TITLE: ") + longTitle + QStringLiteral("\nRESULTS: r\nSUMMARY: s\n");
        const auto p = parseHeartbeatReport(input);
        QVERIFY(p.valid);
        QCOMPARE(p.title.size(), 100);
    }

    void test_crlfTolerance() {
        const QString input = QStringLiteral("TITLE: t\r\n"
                                             "RESULTS:\r\n"
                                             "body\r\n"
                                             "SUMMARY:\r\n"
                                             "s\r\n");
        const auto p = parseHeartbeatReport(input);
        QVERIFY(p.valid);
        QCOMPARE(p.title, QStringLiteral("t"));
        QCOMPARE(p.body, QStringLiteral("body"));
        QCOMPARE(p.summary, QStringLiteral("s"));
    }

    void test_malformedInput_preservesRawInBody() {
        const QString input = QStringLiteral("I am a model that did not follow instructions.");
        const auto p = parseHeartbeatReport(input);
        QVERIFY(!p.valid);
        QVERIFY(p.body.contains(QStringLiteral("did not follow")));
    }

    void test_titleOnly_isNotValid() {
        const QString input = QStringLiteral("TITLE: just a title\n");
        const auto p = parseHeartbeatReport(input);
        QVERIFY(!p.valid);
    }

    void test_summaryOnly_isValid() {
        const QString input = QStringLiteral("SUMMARY: it worked\n");
        const auto p = parseHeartbeatReport(input);
        QVERIFY(p.valid);
        QCOMPARE(p.summary, QStringLiteral("it worked"));
    }

    void test_preambleBeforeFirstLabelIsIgnored() {
        const QString input = QStringLiteral("I'm thinking about what to say...\n"
                                             "Let me start.\n"
                                             "TITLE: t\n"
                                             "RESULTS: r\n"
                                             "SUMMARY: s\n");
        const auto p = parseHeartbeatReport(input);
        QVERIFY(p.valid);
        QVERIFY(!p.body.contains(QStringLiteral("thinking about")));
        QCOMPARE(p.title, QStringLiteral("t"));
    }
};

QTEST_MAIN(TestHeartbeatReportParser)
#include "test-heartbeat-report-parser.moc"
