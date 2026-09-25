// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "services/file-service.h"
#include "tools/file/read-file-tool.h"

#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <QFile>

class TestReadFileTool : public QObject {
    Q_OBJECT

  private slots:
    void test_contract() {
        FileService fs;
        Tools::ReadFileTool tool(fs);
        QCOMPARE(tool.name(), QStringLiteral("read_file"));
        QCOMPARE(tool.runsOnMainThread(), false);
    }

    void test_parameters() {
        FileService fs;
        Tools::ReadFileTool tool(fs);
        const auto params = tool.parameters();
        QCOMPARE(params.size(), 2);
        QCOMPARE(params[0].name, QStringLiteral("path"));
        QCOMPARE(params[0].required, true);
        QCOMPARE(params[1].name, QStringLiteral("max_lines"));
        QCOMPARE(params[1].type, QStringLiteral("integer"));
        QCOMPARE(params[1].required, false);
    }

    void test_invoke_emptyPath_returnsError() {
        FileService fs;
        Tools::ReadFileTool tool(fs);
        QJsonObject args;
        args[QStringLiteral("path")] = QString();
        const QJsonValue result = tool.invoke(args);
        QVERIFY(result.toObject().contains(QStringLiteral("error")));
    }

    void test_invoke_missingFile_returnsError() {
        FileService fs;
        Tools::ReadFileTool tool(fs);
        QJsonObject args;
        args[QStringLiteral("path")] = QStringLiteral("/definitely/nope/xyz.txt");
        const QJsonValue result = tool.invoke(args);
        QVERIFY(result.toObject().contains(QStringLiteral("error")));
    }

    void test_invoke_existingFile_returnsContent() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString path = dir.filePath(QStringLiteral("probe.txt"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("read-tool-direct");
        f.close();

        FileService fs;
        Tools::ReadFileTool tool(fs);
        QJsonObject args;
        args[QStringLiteral("path")] = path;
        const QJsonValue result = tool.invoke(args);

        const QJsonObject obj = result.toObject();
        QCOMPARE(obj[QStringLiteral("path")].toString(), path);
        QCOMPARE(obj[QStringLiteral("content")].toString(), QStringLiteral("read-tool-direct"));
        QCOMPARE(obj[QStringLiteral("size")].toInt(),
                 static_cast<int>(QByteArrayLiteral("read-tool-direct").size()));
    }
};

QTEST_MAIN(TestReadFileTool)
#include "test-read-file-tool.moc"
