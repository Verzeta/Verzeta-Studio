// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "services/file-service.h"
#include "tools/file/list-files-tool.h"

#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <QFile>
#include <QJsonArray>

class TestListFilesTool : public QObject {
    Q_OBJECT

  private slots:
    void test_contract() {
        FileService fs;
        Tools::ListFilesTool tool(fs);
        QCOMPARE(tool.name(), QStringLiteral("list_files"));
        QVERIFY(!tool.description().isEmpty());
        QCOMPARE(tool.runsOnMainThread(), false);
    }

    void test_parameters_pathOptionalRecursiveOptional() {
        FileService fs;
        Tools::ListFilesTool tool(fs);
        const auto params = tool.parameters();
        QCOMPARE(params.size(), 2);
        QCOMPARE(params[0].name, QStringLiteral("path"));
        QCOMPARE(params[0].type, QStringLiteral("string"));
        QCOMPARE(params[0].required, false);
        QCOMPARE(params[1].name, QStringLiteral("recursive"));
        QCOMPARE(params[1].type, QStringLiteral("boolean"));
        QCOMPARE(params[1].required, false);
    }

    void test_invoke_missingDirectory_returnsError() {
        FileService fs;
        Tools::ListFilesTool tool(fs);
        QJsonObject args;
        args[QStringLiteral("path")] = QStringLiteral("/no/such/dir/xyz789abc");
        const QJsonValue result = tool.invoke(args);
        QVERIFY(result.isObject());
        QVERIFY(result.toObject().contains(QStringLiteral("error")));
    }

    void test_invoke_absoluteMissing_withFolderId_returnsError() {
        FileService fs;
        Tools::ListFilesTool tool(fs);
        QJsonObject args;
        args[QStringLiteral("path")] = QStringLiteral("/no/such/abs/qzx123");
        args[QStringLiteral("__caller_folder_id")] = QStringLiteral("folder-abc");
        const QJsonValue result = tool.invoke(args);
        QVERIFY(result.isObject());
        QVERIFY(result.toObject().contains(QStringLiteral("error")));
    }

    void test_invoke_bareRoot_doesNotListFilesystem() {
        FileService fs;
        Tools::ListFilesTool tool(fs);
        QJsonObject args;
        args[QStringLiteral("path")] = QStringLiteral("/");
        args[QStringLiteral("recursive")] = false;
        const QJsonObject obj = tool.invoke(args).toObject();
        const QJsonArray files = obj[QStringLiteral("files")].toArray();
        for (const QJsonValue& v : files) {
            const QString s = v.toString();
            QVERIFY2(
                !s.startsWith(QStringLiteral("/etc")) && !s.startsWith(QStringLiteral("/usr")) &&
                    !s.startsWith(QStringLiteral("/sys")) &&
                    !s.startsWith(QStringLiteral("/bin")) && !s.startsWith(QStringLiteral("/lib")),
                qPrintable(QStringLiteral("bare / leaked filesystem: %1").arg(s)));
        }
    }

    void test_invoke_capsLargeListing_notesTruncation() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const int n = FileService::kMaxListEntries + 25;
        for (int i = 0; i < n; ++i) {
            QFile f(dir.filePath(QStringLiteral("f%1.txt").arg(i)));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("x");
            f.close();
        }
        FileService fs;
        Tools::ListFilesTool tool(fs);
        QJsonObject args;
        args[QStringLiteral("path")] = dir.path();
        const QJsonObject obj = tool.invoke(args).toObject();
        QCOMPARE(obj[QStringLiteral("files")].toArray().size(), FileService::kMaxListEntries);
        QVERIFY(obj[QStringLiteral("truncated")].toBool());
    }

    void test_invoke_existingDirectory_nonRecursive_returnsFiles() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile f(dir.filePath(QStringLiteral("a.txt")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("x");
        f.close();

        FileService fs;
        Tools::ListFilesTool tool(fs);
        QJsonObject args;
        args[QStringLiteral("path")] = dir.path();

        const QJsonValue result = tool.invoke(args);
        QVERIFY(result.isObject());
        const QJsonObject obj = result.toObject();
        QVERIFY(!obj.contains(QStringLiteral("path")));
        QVERIFY(obj[QStringLiteral("files")].isArray());
        QCOMPARE(obj[QStringLiteral("files")].toArray().size(), 1);
    }

    void test_invoke_recursive_walksSubdirectories() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(QDir().mkpath(dir.filePath(QStringLiteral("sub"))));
        {
            QFile f(dir.filePath(QStringLiteral("sub/inner.txt")));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("y");
            f.close();
        }

        FileService fs;
        Tools::ListFilesTool tool(fs);
        QJsonObject args;
        args[QStringLiteral("path")] = dir.path();
        args[QStringLiteral("recursive")] = true;

        const QJsonValue result = tool.invoke(args);
        QVERIFY(result.isObject());
        const QJsonArray files = result.toObject()[QStringLiteral("files")].toArray();
        QVERIFY(files.size() >= 2);
    }
};

QTEST_MAIN(TestListFilesTool)
#include "test-list-files-tool.moc"
