// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "api/tool-calling-schema.h"
#include "services/file-service.h"
#include "services/tool-service.h"
#include "utils/process-sandbox.h"

#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

class TestToolService : public QObject {
    Q_OBJECT

  private slots:

    void test_freshService_noTools() {
        ToolService svc;
        QVERIFY(svc.availableTools().isEmpty());
    }

    void test_registerTool_hasTool() {
        ToolService svc;

        ToolSchema schema;
        schema.name = QStringLiteral("custom_tool");
        schema.description = QStringLiteral("A custom test tool");

        bool handlerCalled = false;
        svc.registerTool(schema, [&handlerCalled](const QJsonObject&) -> QJsonValue {
            handlerCalled = true;
            return QStringLiteral("ok");
        });

        QVERIFY(svc.hasTool(QStringLiteral("custom_tool")));
        QCOMPARE(svc.availableTools().size(), 1);

        svc.invokeTool(QStringLiteral("custom_tool"), {});
        QVERIFY(handlerCalled);
    }

    void test_invokeTool_unknownTool_emitsError() {
        ToolService svc;

        QStringList errorNames;
        connect(&svc, &ToolService::toolError, this, [&](const QString& name, const QString&) {
            errorNames.append(name);
        });

        const QJsonValue result = svc.invokeTool(QStringLiteral("nonexistent_tool"), {});

        QCOMPARE(errorNames.count(), 1);
        QCOMPARE(errorNames.first(), QStringLiteral("nonexistent_tool"));
        QVERIFY(result.isObject());
        QVERIFY(result.toObject().contains(QStringLiteral("error")));
    }

    void test_registerBuiltInTools_registersAll6() {
        ToolService svc;
        ProcessSandbox sandbox;
        FileService fileSvc;

        svc.registerBuiltInTools(sandbox, fileSvc);

        QVERIFY(svc.hasTool(QStringLiteral("run_shell")));
        QVERIFY(svc.hasTool(QStringLiteral("list_files")));
        QVERIFY(svc.hasTool(QStringLiteral("read_file")));
        QVERIFY(svc.hasTool(QStringLiteral("write_file")));
        QVERIFY(svc.hasTool(QStringLiteral("get_current_time")));
        QVERIFY(svc.hasTool(QStringLiteral("search_web")));
        QVERIFY(svc.hasTool(QStringLiteral("request_turn")));
        QCOMPARE(svc.availableTools().size(), 7);
    }

    void test_builtIn_getCurrentTime_returnsDatetime() {
        ToolService svc;
        ProcessSandbox sandbox;
        FileService fileSvc;
        svc.registerBuiltInTools(sandbox, fileSvc);

        const QJsonValue result = svc.invokeTool(QStringLiteral("get_current_time"), {});

        QVERIFY(result.isObject());
        const QJsonObject obj = result.toObject();
        QVERIFY(obj.contains(QStringLiteral("datetime")));
        QVERIFY(!obj[QStringLiteral("datetime")].toString().isEmpty());
    }

    void test_builtIn_listFiles_validDir_returnsFilesArray() {
        ToolService svc;
        ProcessSandbox sandbox;
        FileService fileSvc;
        svc.registerBuiltInTools(sandbox, fileSvc);

        QJsonObject args;
        args[QStringLiteral("path")] = QDir::tempPath();

        const QJsonValue result = svc.invokeTool(QStringLiteral("list_files"), args);

        QVERIFY(result.isObject());
        const QJsonObject obj = result.toObject();
        QVERIFY(obj.contains(QStringLiteral("files")));
        QVERIFY(obj[QStringLiteral("files")].isArray());
    }

    void test_builtIn_listFiles_nonExistentDir_returnsError() {
        ToolService svc;
        ProcessSandbox sandbox;
        FileService fileSvc;
        svc.registerBuiltInTools(sandbox, fileSvc);

        QJsonObject args;
        args[QStringLiteral("path")] = QStringLiteral("/no/such/directory/xyz123");

        const QJsonValue result = svc.invokeTool(QStringLiteral("list_files"), args);

        QVERIFY(result.isObject());
        QVERIFY(result.toObject().contains(QStringLiteral("error")));
    }

    void test_toolSchema_toOpenAIFunction_structure() {
        ToolParameterSchema param;
        param.name = QStringLiteral("query");
        param.type = QStringLiteral("string");
        param.description = QStringLiteral("The search query");
        param.required = true;

        ToolSchema schema;
        schema.name = QStringLiteral("search_web");
        schema.description = QStringLiteral("Search the web");
        schema.parameters = {param};

        const QJsonObject obj = schema.toOpenAIFunction();

        QCOMPARE(obj[QStringLiteral("type")].toString(), QStringLiteral("function"));
        QVERIFY(obj.contains(QStringLiteral("function")));

        const QJsonObject fn = obj[QStringLiteral("function")].toObject();
        QCOMPARE(fn[QStringLiteral("name")].toString(), QStringLiteral("search_web"));
        QVERIFY(fn.contains(QStringLiteral("parameters")));

        const QJsonObject params = fn[QStringLiteral("parameters")].toObject();
        QCOMPARE(params[QStringLiteral("type")].toString(), QStringLiteral("object"));
        QVERIFY(params.contains(QStringLiteral("properties")));
        QVERIFY(params.contains(QStringLiteral("required")));

        const QJsonArray required = params[QStringLiteral("required")].toArray();
        QVERIFY(required.contains(QStringLiteral("query")));
    }

    void test_toolSchema_toAnthropicTool_usesInputSchema() {
        ToolSchema schema;
        schema.name = QStringLiteral("read_file");
        schema.description = QStringLiteral("Reads a file");

        ToolParameterSchema p;
        p.name = QStringLiteral("path");
        p.type = QStringLiteral("string");
        p.description = QStringLiteral("File path");
        p.required = true;
        schema.parameters = {p};

        const QJsonObject obj = schema.toAnthropicTool();

        QCOMPARE(obj[QStringLiteral("name")].toString(), QStringLiteral("read_file"));
        QVERIFY(obj.contains(QStringLiteral("input_schema")));
        QVERIFY(!obj.contains(QStringLiteral("parameters")));

        const QJsonObject inputSchema = obj[QStringLiteral("input_schema")].toObject();
        QCOMPARE(inputSchema[QStringLiteral("type")].toString(), QStringLiteral("object"));
    }

    void test_builtIn_runShell_echoCommand_returnsStructuredResult() {
        ToolService svc;
        ProcessSandbox sandbox;
        FileService fileSvc;
        svc.registerBuiltInTools(sandbox, fileSvc);

        QJsonObject args;
        args[QStringLiteral("command")] = QStringLiteral("echo hello-2a");

        const QJsonValue result = svc.invokeTool(QStringLiteral("run_shell"), args);

        QVERIFY(result.isObject());
        const QJsonObject obj = result.toObject();
        QVERIFY(obj.contains(QStringLiteral("stdout")));
        QVERIFY(obj.contains(QStringLiteral("stderr")));
        QVERIFY(obj.contains(QStringLiteral("exitCode")));
        QVERIFY(obj.contains(QStringLiteral("timedOut")));
        QCOMPARE(obj[QStringLiteral("exitCode")].toInt(), 0);
        QCOMPARE(obj[QStringLiteral("timedOut")].toBool(), false);
        QVERIFY(obj[QStringLiteral("stdout")].toString().contains(QStringLiteral("hello-2a")));
    }

    void test_builtIn_runShell_emptyCommand_returnsError() {
        ToolService svc;
        ProcessSandbox sandbox;
        FileService fileSvc;
        svc.registerBuiltInTools(sandbox, fileSvc);

        QJsonObject args;
        args[QStringLiteral("command")] = QString();

        const QJsonValue result = svc.invokeTool(QStringLiteral("run_shell"), args);

        QVERIFY(result.isObject());
        QVERIFY(result.toObject().contains(QStringLiteral("error")));
    }

    void test_builtIn_getCurrentTime_datetimeIsIso8601() {
        ToolService svc;
        ProcessSandbox sandbox;
        FileService fileSvc;
        svc.registerBuiltInTools(sandbox, fileSvc);

        const QJsonValue result = svc.invokeTool(QStringLiteral("get_current_time"), {});
        QVERIFY(result.isObject());
        const QString iso = result.toObject().value(QStringLiteral("datetime")).toString();
        QVERIFY(!iso.isEmpty());
        QVERIFY(QDateTime::fromString(iso, Qt::ISODate).isValid());
    }

    void test_builtIn_requestTurn_validAlias_returnsQueued() {
        ToolService svc;
        ProcessSandbox sandbox;
        FileService fileSvc;
        svc.registerBuiltInTools(sandbox, fileSvc);

        QJsonObject args;
        args[QStringLiteral("alias")] = QStringLiteral("Alice");

        const QJsonValue result = svc.invokeTool(QStringLiteral("request_turn"), args);

        QVERIFY(result.isObject());
        const QJsonObject obj = result.toObject();
        QCOMPARE(obj[QStringLiteral("requested")].toString(), QStringLiteral("Alice"));
        QCOMPARE(obj[QStringLiteral("status")].toString(), QStringLiteral("queued"));
    }

    void test_builtIn_requestTurn_emptyAlias_returnsError() {
        ToolService svc;
        ProcessSandbox sandbox;
        FileService fileSvc;
        svc.registerBuiltInTools(sandbox, fileSvc);

        QJsonObject args;
        args[QStringLiteral("alias")] = QString();

        const QJsonValue result = svc.invokeTool(QStringLiteral("request_turn"), args);

        QVERIFY(result.isObject());
        QVERIFY(result.toObject().contains(QStringLiteral("error")));
    }

    void test_builtIn_requestTurn_broadcastAlias_preserved() {
        ToolService svc;
        ProcessSandbox sandbox;
        FileService fileSvc;
        svc.registerBuiltInTools(sandbox, fileSvc);

        QJsonObject args;
        args[QStringLiteral("alias")] = QStringLiteral("all");

        const QJsonValue result = svc.invokeTool(QStringLiteral("request_turn"), args);

        QVERIFY(result.isObject());
        QCOMPARE(result.toObject()[QStringLiteral("requested")].toString(), QStringLiteral("all"));
    }

    void test_builtIn_readFile_existingFile_returnsContentAndSize() {
        ToolService svc;
        ProcessSandbox sandbox;
        FileService fileSvc;
        svc.registerBuiltInTools(sandbox, fileSvc);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString filePath = dir.filePath(QStringLiteral("hello.txt"));
        {
            QFile f(filePath);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("read-file-chars");
            f.close();
        }

        QJsonObject args;
        args[QStringLiteral("path")] = filePath;
        const QJsonValue result = svc.invokeTool(QStringLiteral("read_file"), args);

        QVERIFY(result.isObject());
        const QJsonObject obj = result.toObject();
        QCOMPARE(obj[QStringLiteral("path")].toString(), filePath);
        QCOMPARE(obj[QStringLiteral("content")].toString(), QStringLiteral("read-file-chars"));
        QCOMPARE(obj[QStringLiteral("size")].toInt(),
                 static_cast<int>(QByteArrayLiteral("read-file-chars").size()));
    }

    void test_builtIn_readFile_emptyPath_returnsError() {
        ToolService svc;
        ProcessSandbox sandbox;
        FileService fileSvc;
        svc.registerBuiltInTools(sandbox, fileSvc);

        QJsonObject args;
        args[QStringLiteral("path")] = QString();
        const QJsonValue result = svc.invokeTool(QStringLiteral("read_file"), args);

        QVERIFY(result.isObject());
        QVERIFY(result.toObject().contains(QStringLiteral("error")));
    }

    void test_builtIn_readFile_missingFile_returnsError() {
        ToolService svc;
        ProcessSandbox sandbox;
        FileService fileSvc;
        svc.registerBuiltInTools(sandbox, fileSvc);

        QJsonObject args;
        args[QStringLiteral("path")] = QStringLiteral("/definitely/does/not/exist/xyz123.txt");
        const QJsonValue result = svc.invokeTool(QStringLiteral("read_file"), args);

        QVERIFY(result.isObject());
        QVERIFY(result.toObject().contains(QStringLiteral("error")));
    }

    void test_builtIn_writeFile_happyPath_createsFile() {
        ToolService svc;
        ProcessSandbox sandbox;
        FileService fileSvc;
        svc.registerBuiltInTools(sandbox, fileSvc);

        fileSvc.setActiveConversation(QStringLiteral("write-file-char-test"));

        QJsonObject args;
        args[QStringLiteral("filename")] = QStringLiteral("sample-note.txt");
        args[QStringLiteral("content")] = QStringLiteral("phase-2b-probe");
        const QJsonValue result = svc.invokeTool(QStringLiteral("write_file"), args);

        QVERIFY(result.isObject());
        const QJsonObject obj = result.toObject();
        QCOMPARE(obj[QStringLiteral("path")].toString(), QStringLiteral("sample-note.txt"));
        QCOMPARE(obj[QStringLiteral("written")].toInt(),
                 static_cast<int>(QStringLiteral("phase-2b-probe").size()));

        const QString resolved =
            fileSvc.activeProjectDir() + QLatin1Char('/') + QStringLiteral("sample-note.txt");
        QFile written(resolved);
        QVERIFY(written.exists());
        QVERIFY(written.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(written.readAll()), QStringLiteral("phase-2b-probe"));
        written.remove();
    }

    void test_builtIn_writeFile_emptyFilename_returnsError() {
        ToolService svc;
        ProcessSandbox sandbox;
        FileService fileSvc;
        svc.registerBuiltInTools(sandbox, fileSvc);

        QJsonObject args;
        args[QStringLiteral("filename")] = QString();
        args[QStringLiteral("content")] = QStringLiteral("x");
        const QJsonValue result = svc.invokeTool(QStringLiteral("write_file"), args);

        QVERIFY(result.isObject());
        QVERIFY(result.toObject().contains(QStringLiteral("error")));
    }

    void test_builtIn_writeFile_emptyContent_returnsError() {
        ToolService svc;
        ProcessSandbox sandbox;
        FileService fileSvc;
        svc.registerBuiltInTools(sandbox, fileSvc);

        QJsonObject args;
        args[QStringLiteral("filename")] = QStringLiteral("x.txt");
        args[QStringLiteral("content")] = QString();
        const QJsonValue result = svc.invokeTool(QStringLiteral("write_file"), args);

        QVERIFY(result.isObject());
        QVERIFY(result.toObject().contains(QStringLiteral("error")));
    }

    void test_builtIn_searchWeb_emptyQuery_returnsError() {
        ToolService svc;
        ProcessSandbox sandbox;
        FileService fileSvc;
        svc.registerBuiltInTools(sandbox, fileSvc);

        QJsonObject args;
        args[QStringLiteral("query")] = QString();
        const QJsonValue result = svc.invokeTool(QStringLiteral("search_web"), args);

        QVERIFY(result.isObject());
        QVERIFY(result.toObject().contains(QStringLiteral("error")));
    }

    void test_toolSchema_toGeminiFunction_uppercaseTypes() {
        ToolSchema schema;
        schema.name = QStringLiteral("list_files");
        schema.description = QStringLiteral("Lists directory entries");

        ToolParameterSchema p;
        p.name = QStringLiteral("path");
        p.type = QStringLiteral("string");
        p.description = QStringLiteral("Directory path");
        p.required = true;
        schema.parameters = {p};

        const QJsonObject obj = schema.toGeminiFunction();

        QCOMPARE(obj[QStringLiteral("name")].toString(), QStringLiteral("list_files"));
        QVERIFY(obj.contains(QStringLiteral("parameters")));

        const QJsonObject params = obj[QStringLiteral("parameters")].toObject();
        QCOMPARE(params[QStringLiteral("type")].toString(), QStringLiteral("OBJECT"));

        const QJsonObject props = params[QStringLiteral("properties")].toObject();
        QVERIFY(props.contains(QStringLiteral("path")));
        QCOMPARE(props[QStringLiteral("path")].toObject()[QStringLiteral("type")].toString(),
                 QStringLiteral("STRING"));
    }


    void test_runsOnMainThread_unknownName_returnsFalse() {
        ToolService svc;
        QCOMPARE(svc.runsOnMainThread(QStringLiteral("nope")), false);
    }

    void test_runsOnMainThread_plainRegistration_defaultsFalse() {
        ToolService svc;
        ToolSchema schema;
        schema.name = QStringLiteral("plain_tool");
        schema.description = QStringLiteral("A direct-handler registration");
        svc.registerTool(schema, [](const QJsonObject&) -> QJsonValue { return QJsonValue{}; });
        QCOMPARE(svc.runsOnMainThread(QStringLiteral("plain_tool")), false);
    }

    void test_setRunsOnMainThread_roundTrip() {
        ToolService svc;
        ToolSchema schema;
        schema.name = QStringLiteral("plain_tool");
        svc.registerTool(schema, [](const QJsonObject&) -> QJsonValue { return QJsonValue{}; });

        svc.setRunsOnMainThread(QStringLiteral("plain_tool"), true);
        QCOMPARE(svc.runsOnMainThread(QStringLiteral("plain_tool")), true);
        svc.setRunsOnMainThread(QStringLiteral("plain_tool"), false);
        QCOMPARE(svc.runsOnMainThread(QStringLiteral("plain_tool")), false);

        svc.setRunsOnMainThread(QStringLiteral("no_such_tool"), true);
        QCOMPARE(svc.hasTool(QStringLiteral("no_such_tool")), false);
    }

    void test_registerBuiltInTools_shellAndFileAndWebAreWorkerSafe() {
        ToolService svc;
        ProcessSandbox sandbox;
        FileService fileSvc;
        svc.registerBuiltInTools(sandbox, fileSvc);

        for (const QString& name : {
                 QStringLiteral("run_shell"),
                 QStringLiteral("list_files"),
                 QStringLiteral("read_file"),
                 QStringLiteral("write_file"),
                 QStringLiteral("get_current_time"),
                 QStringLiteral("search_web"),
                 QStringLiteral("request_turn"),
             }) {
            QVERIFY2(!svc.runsOnMainThread(name),
                     qPrintable(QStringLiteral("%1 must be worker-safe (not main-thread) "
                                               "after the runsOnMainThread migration.")
                                    .arg(name)));
        }
    }
};

QTEST_MAIN(TestToolService)
#include "test-tool-service.moc"
