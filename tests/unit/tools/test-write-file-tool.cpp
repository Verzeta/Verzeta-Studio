// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "services/file-service.h"
#include "tools/file/write-file-tool.h"

#include <QtTest/QtTest>

#include <QFile>

class TestWriteFileTool : public QObject {
    Q_OBJECT

  private slots:
    void test_contract() {
        FileService fs;
        Tools::WriteFileTool tool(fs);
        QCOMPARE(tool.name(), QStringLiteral("write_file"));
        QCOMPARE(tool.runsOnMainThread(), false);
    }

    void test_parameters() {
        FileService fs;
        Tools::WriteFileTool tool(fs);
        const auto params = tool.parameters();
        QCOMPARE(params.size(), 2);
        QCOMPARE(params[0].name, QStringLiteral("filename"));
        QCOMPARE(params[0].required, true);
        QCOMPARE(params[1].name, QStringLiteral("content"));
        QCOMPARE(params[1].required, true);
    }

    void test_invoke_emptyFilename_returnsError() {
        FileService fs;
        Tools::WriteFileTool tool(fs);
        QJsonObject args;
        args[QStringLiteral("filename")] = QString();
        args[QStringLiteral("content")] = QStringLiteral("x");
        const QJsonValue result = tool.invoke(args);
        QVERIFY(result.toObject().contains(QStringLiteral("error")));
    }

    void test_invoke_emptyContent_returnsError() {
        FileService fs;
        Tools::WriteFileTool tool(fs);
        QJsonObject args;
        args[QStringLiteral("filename")] = QStringLiteral("x.txt");
        args[QStringLiteral("content")] = QString();
        const QJsonValue result = tool.invoke(args);
        QVERIFY(result.toObject().contains(QStringLiteral("error")));
    }

    void test_invoke_writesFile_responseEchoesInputFilename() {
        FileService fs;
        fs.setActiveConversation(QStringLiteral("write-file-tool-direct"));

        Tools::WriteFileTool tool(fs);
        QJsonObject args;
        args[QStringLiteral("filename")] = QStringLiteral("probe.txt");
        args[QStringLiteral("content")] = QStringLiteral("direct-write");
        const QJsonValue result = tool.invoke(args);

        const QJsonObject obj = result.toObject();
        QCOMPARE(obj[QStringLiteral("path")].toString(), QStringLiteral("probe.txt"));
        QCOMPARE(obj[QStringLiteral("written")].toInt(),
                 static_cast<int>(QStringLiteral("direct-write").size()));

        const QString resolved =
            fs.activeProjectDir() + QLatin1Char('/') + QStringLiteral("probe.txt");
        QFile f(resolved);
        QVERIFY(f.exists());
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(f.readAll()), QStringLiteral("direct-write"));
        f.remove();
    }

    void test_invoke_doubleEscapedContent_repairedOnDisk() {
        FileService fs;
        fs.setActiveConversation(QStringLiteral("write-file-tool-repair"));

        Tools::WriteFileTool tool(fs);
        QJsonObject args;
        args[QStringLiteral("filename")] = QStringLiteral("repair.md");
        args[QStringLiteral("content")] =
            QStringLiteral("## Title\\n\\nIntro paragraph.\\n\\n- a\\n- b\\n\\nEnd.");
        const QJsonValue result = tool.invoke(args);
        QVERIFY(!result.toObject().contains(QStringLiteral("error")));

        const QString resolved =
            fs.activeProjectDir() + QLatin1Char('/') + QStringLiteral("repair.md");
        QFile f(resolved);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString onDisk = QString::fromUtf8(f.readAll());
        QVERIFY(!onDisk.contains(QStringLiteral("\\n")));
        QCOMPARE(onDisk.count(QLatin1Char('\n')), 7);
        QVERIFY(onDisk.startsWith(QStringLiteral("## Title\n\nIntro")));
        f.remove();
    }
};

QTEST_MAIN(TestWriteFileTool)
#include "test-write-file-tool.moc"
