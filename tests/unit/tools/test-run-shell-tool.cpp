// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "services/background-process-service.h"
#include "services/file-service.h"
#include "tools/shell/run-shell-tool.h"
#include "utils/process-sandbox.h"

#include <QtTest/QtTest>

class TestRunShellTool : public QObject {
    Q_OBJECT

  private slots:
    void test_contract_nameDescriptionThreadResidency() {
        ProcessSandbox sandbox;
        FileService fileService;
        Tools::RunShellTool tool(sandbox, fileService);

        QCOMPARE(tool.name(), QStringLiteral("run_shell"));
        QVERIFY(!tool.description().isEmpty());
        QVERIFY(tool.description().contains(QStringLiteral("refused"), Qt::CaseInsensitive));
        QCOMPARE(tool.runsOnMainThread(), false);
    }

    void test_parameters_requiredCommandPlusOptionalBackground() {
        ProcessSandbox sandbox;
        FileService fileService;
        Tools::RunShellTool tool(sandbox, fileService);

        const auto params = tool.parameters();
        QCOMPARE(params.size(), 2);
        QCOMPARE(params.at(0).name, QStringLiteral("command"));
        QCOMPARE(params.at(0).type, QStringLiteral("string"));
        QCOMPARE(params.at(0).required, true);
        QCOMPARE(params.at(1).name, QStringLiteral("background"));
        QCOMPARE(params.at(1).type, QStringLiteral("boolean"));
        QCOMPARE(params.at(1).required, false);
    }

    void test_invoke_emptyCommand_returnsErrorObject() {
        ProcessSandbox sandbox;
        FileService fileService;
        Tools::RunShellTool tool(sandbox, fileService);

        QJsonObject args;
        args[QStringLiteral("command")] = QString();

        const QJsonValue result = tool.invoke(args);
        QVERIFY(result.isObject());
        QVERIFY(result.toObject().contains(QStringLiteral("error")));
    }

    void test_invoke_echoCommand_returnsStructuredResult() {
        ProcessSandbox sandbox;
        FileService fileService;
        Tools::RunShellTool tool(sandbox, fileService);

        QJsonObject args;
        args[QStringLiteral("command")] = QStringLiteral("echo direct-invoke");

        const QJsonValue result = tool.invoke(args);
        QVERIFY(result.isObject());
        const QJsonObject obj = result.toObject();
        QVERIFY(obj.contains(QStringLiteral("stdout")));
        QVERIFY(obj.contains(QStringLiteral("stderr")));
        QVERIFY(obj.contains(QStringLiteral("exitCode")));
        QVERIFY(obj.contains(QStringLiteral("timedOut")));
        QCOMPARE(obj[QStringLiteral("exitCode")].toInt(), 0);
        QCOMPARE(obj[QStringLiteral("timedOut")].toBool(), false);
        QVERIFY(obj[QStringLiteral("stdout")].toString().contains(QStringLiteral("direct-invoke")));
    }

    void test_defaultAllowList_networkAndProcessControl() {
        const QStringList def = ProcessSandbox::defaultAllowList();
#ifndef Q_OS_ANDROID
        QVERIFY2(def.contains(QStringLiteral("curl")), "curl must be allow-listed by default");
        ProcessSandbox sandbox;
        QCOMPARE(sandbox.allowList(), def);
        QVERIFY(sandbox.isCommandAllowed(QStringLiteral("curl -s http://localhost:5173")));
#endif
#if !defined(Q_OS_WIN) && !defined(Q_OS_ANDROID)
        QVERIFY(def.contains(QStringLiteral("wget")));
        QVERIFY(def.contains(QStringLiteral("kill")));
        QVERIFY(def.contains(QStringLiteral("pkill")));
        QVERIFY(def.contains(QStringLiteral("npm")));
        QVERIFY(def.contains(QStringLiteral("python3")));
#endif
    }

    void test_setAllowList_overrideCanRemoveDefault() {
        ProcessSandbox sandbox;
        sandbox.setAllowList({QStringLiteral("ls"), QStringLiteral("echo")});
        QVERIFY(!sandbox.isCommandAllowed(QStringLiteral("curl http://x")));
        QVERIFY(sandbox.isCommandAllowed(QStringLiteral("ls -la")));
    }

    void test_invoke_background_returnsHandleAndOutput() {
        ProcessSandbox sandbox;
        FileService fileService;
        BackgroundProcessService bg(sandbox);
        Tools::RunShellTool tool(sandbox, fileService, &bg);

        QJsonObject args;
        args[QStringLiteral("command")] = QStringLiteral("echo bg-hello");
        args[QStringLiteral("background")] = true;

        const QJsonObject obj = tool.invoke(args).toObject();
        QVERIFY(obj.value(QStringLiteral("background")).toBool());
        QVERIFY(!obj.value(QStringLiteral("id")).toString().isEmpty());
        QVERIFY(!obj.value(QStringLiteral("log")).toString().isEmpty());
        QVERIFY(
            obj.value(QStringLiteral("stdout")).toString().contains(QStringLiteral("bg-hello")));
    }

    void test_invoke_background_withoutService_errors() {
        ProcessSandbox sandbox;
        FileService fileService;
        Tools::RunShellTool tool(sandbox, fileService);

        QJsonObject args;
        args[QStringLiteral("command")] = QStringLiteral("echo x");
        args[QStringLiteral("background")] = true;

        const QJsonObject obj = tool.invoke(args).toObject();
        QVERIFY(obj.contains(QStringLiteral("error")));
    }

    void test_invoke_background_nonMountFolderId_runsLocally() {
        ProcessSandbox sandbox;
        FileService fileService;
        BackgroundProcessService bg(sandbox);
        Tools::RunShellTool tool(sandbox, fileService, &bg);

        QJsonObject args;
        args[QStringLiteral("command")] = QStringLiteral("echo bg-local");
        args[QStringLiteral("background")] = true;
        args[QStringLiteral("__caller_folder_id")] = QStringLiteral("proj-folder-123");

        const QJsonObject obj = tool.invoke(args).toObject();
        QVERIFY2(!obj.contains(QStringLiteral("error")),
                 qPrintable(obj.value(QStringLiteral("error")).toString()));
        QVERIFY(obj.value(QStringLiteral("background")).toBool());
        QVERIFY(
            obj.value(QStringLiteral("stdout")).toString().contains(QStringLiteral("bg-local")));
    }
};

QTEST_MAIN(TestRunShellTool)
#include "test-run-shell-tool.moc"
