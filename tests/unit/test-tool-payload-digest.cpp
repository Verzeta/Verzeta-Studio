// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/services/chat/tool-payload-digest.h"

#include <QtTest>

#include <QJsonObject>

using Chat::ToolPayloadDigest;

class TestToolPayloadDigest : public QObject {
    Q_OBJECT

  private slots:
    void test_digestArgs_writeFile_stripsContentKeepsFilename() {
        QJsonObject args;
        args[QStringLiteral("filename")] = QStringLiteral("index.html");
        args[QStringLiteral("content")] = QStringLiteral("a\nb\nc");
        const QJsonObject d = ToolPayloadDigest::digestArgs(QStringLiteral("write_file"), args);

        QVERIFY2(!d.contains(QStringLiteral("content")),
                 "full body must be stripped from history args");
        QCOMPARE(d.value(QStringLiteral("filename")).toString(), QStringLiteral("index.html"));
        QVERIFY(ToolPayloadDigest::isDigestedArgs(d));
        const QString summary = d.value(QStringLiteral("__digest")).toString();
        QVERIFY(summary.contains(QStringLiteral("index.html")));
        QVERIFY(summary.contains(QStringLiteral("3 lines")));
        QVERIFY(summary.contains(QStringLiteral("5 B")));
    }

    void test_digestArgs_idempotent() {
        QJsonObject args;
        args[QStringLiteral("filename")] = QStringLiteral("f.txt");
        args[QStringLiteral("content")] = QStringLiteral("x\ny");
        const QJsonObject once = ToolPayloadDigest::digestArgs(QStringLiteral("write_file"), args);
        const QJsonObject twice = ToolPayloadDigest::digestArgs(QStringLiteral("write_file"), once);
        QCOMPARE(twice, once);
    }

    void test_digestArgs_generic_elidesOversizedField() {
        QJsonObject args;
        args[QStringLiteral("command")] = QString(400, QLatin1Char('x'));
        const QJsonObject d = ToolPayloadDigest::digestArgs(QStringLiteral("run_shell"), args);
        QVERIFY(ToolPayloadDigest::isDigestedArgs(d));
        QVERIFY2(d.value(QStringLiteral("command")).toString().contains(QStringLiteral("elided")),
                 "oversized non-body field must be elided");
        QVERIFY(d.value(QStringLiteral("command")).toString().length() < 400);
    }

    void test_digestArgs_keepsShortScalars() {
        QJsonObject args;
        args[QStringLiteral("mode")] = QStringLiteral("overwrite");
        args[QStringLiteral("content")] = QStringLiteral("body\nbody");
        const QJsonObject d = ToolPayloadDigest::digestArgs(QStringLiteral("write_file"), args);
        QCOMPARE(d.value(QStringLiteral("mode")).toString(), QStringLiteral("overwrite"));
    }

    void test_digestResult_searchWeb_keepsAnswerAndSourcesDropsSnippets() {
        const QString result =
            QStringLiteral("{\"answer\":\"AES-256 is strong\",\"results\":["
                           "{\"title\":\"T1\",\"url\":\"u1\",\"snippet\":\"LONGSNIPPETBODY\"},"
                           "{\"title\":\"T2\",\"url\":\"u2\",\"snippet\":\"more\"}]}");
        const QString d = ToolPayloadDigest::digestResult(QStringLiteral("search_web"), result);
        QVERIFY(ToolPayloadDigest::isDigestedResult(d));
        QVERIFY(d.contains(QStringLiteral("AES-256 is strong")));
        QVERIFY(d.contains(QStringLiteral("T1 (u1)")));
        QVERIFY2(!d.contains(QStringLiteral("LONGSNIPPETBODY")),
                 "raw snippet body must be dropped from history");
    }

    void test_digestResult_readFile_keepsSnippetNotCircular() {
        const QString body =
            QStringLiteral("IMPORTANT_FETCHED_LINE_1\n") + QString(800, QLatin1Char('q'));
        const QString result =
            QStringLiteral("{\"path\":\"a.txt\",\"total_lines\":42,\"content\":\"%1\"}").arg(body);
        const QString d = ToolPayloadDigest::digestResult(QStringLiteral("read_file"), result);
        QVERIFY(ToolPayloadDigest::isDigestedResult(d));
        QVERIFY(d.contains(QStringLiteral("a.txt")));
        QVERIFY(d.contains(QStringLiteral("42")));
        QVERIFY2(d.contains(QStringLiteral("IMPORTANT_FETCHED_LINE_1")),
                 "digested read must keep a leading snippet of the real body");
        QVERIFY2(!d.contains(QStringLiteral("re-read to view content")),
                 "the circular re-read instruction must be gone");
        QVERIFY(d.length() < result.length());
    }

    void test_digestResult_genericOversized_elided() {
        const QString result = QString(500, QLatin1Char('z'));
        const QString d = ToolPayloadDigest::digestResult(QStringLiteral("some_tool"), result);
        QVERIFY(ToolPayloadDigest::isDigestedResult(d));
        QVERIFY(d.contains(QStringLiteral("elided")));
        QVERIFY(d.length() < 500);
    }

    void test_digestResult_shortKeptVerbatim() {
        const QString result = QStringLiteral("ok");
        const QString d = ToolPayloadDigest::digestResult(QStringLiteral("some_tool"), result);
        QCOMPARE(d, QStringLiteral("ok"));
        QVERIFY(!ToolPayloadDigest::isDigestedResult(d));
    }

    void test_digestResult_idempotent() {
        const QString result =
            QStringLiteral("{\"answer\":\"x\",\"results\":[{\"title\":\"T\",\"url\":\"u\"}]}");
        const QString once = ToolPayloadDigest::digestResult(QStringLiteral("search_web"), result);
        const QString twice = ToolPayloadDigest::digestResult(QStringLiteral("search_web"), once);
        QCOMPARE(twice, once);
    }
};

QTEST_APPLESS_MAIN(TestToolPayloadDigest)
#include "test-tool-payload-digest.moc"
