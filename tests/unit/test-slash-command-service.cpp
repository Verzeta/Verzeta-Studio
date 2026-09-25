// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "api/tool-calling-schema.h"
#include "services/slash-command-service.h"
#include "services/tool-service.h"

#include <QTest>

#include <memory>
#include <QJsonObject>
#include <QVariantMap>

class TestSlashCommandService : public QObject {
    Q_OBJECT

  private:
    std::unique_ptr<SlashCommandService> m_handler;
    std::unique_ptr<ToolService> m_toolSvc;

    QStringList m_postedMessages;
    int m_reloadCalls = 0;

    void registerFixtureTools() {
        auto noopHandler = [](const QJsonObject&) -> QJsonValue { return QJsonValue{}; };
        ToolSchema a;
        a.name = QStringLiteral("run_shell");
        a.description = QStringLiteral("Execute a shell command");
        m_toolSvc->registerTool(a, noopHandler, ToolKind::BuiltIn);
        ToolSchema b;
        b.name = QStringLiteral("read_file");
        b.description = QStringLiteral("Read a local file");
        m_toolSvc->registerTool(b, noopHandler, ToolKind::BuiltIn);
        ToolSchema c;
        c.name = QStringLiteral("my_custom_tool");
        c.description = QStringLiteral("A user-defined command");
        m_toolSvc->registerTool(c, noopHandler, ToolKind::Custom);
        ToolSchema d;
        d.name = QStringLiteral("slackmcp:send_message");
        d.description = QStringLiteral("Post to a Slack channel");
        m_toolSvc->registerTool(d, noopHandler, ToolKind::Mcp);
        ToolSchema e;
        e.name = QStringLiteral("slackmcp:list_channels");
        e.description = QStringLiteral("List Slack channels");
        m_toolSvc->registerTool(e, noopHandler, ToolKind::Mcp);
    }

    SlashCommandContext baseContext(const QString& text) {
        SlashCommandContext ctx;
        ctx.text = text;
        ctx.activeConvId = QStringLiteral("test-conv-id");
        ctx.toolService = m_toolSvc.get();
        ctx.postMessage = [this](const QString& content) { m_postedMessages.append(content); };
        ctx.reloadActiveConversation = [this]() { ++m_reloadCalls; };
        ctx.resolveArtifactsLocation = []() { return SlashCommandContext::ArtifactsLocation{}; };
        return ctx;
    }

  private slots:
    void init() {
        m_handler = std::make_unique<SlashCommandService>();
        m_toolSvc = std::make_unique<ToolService>();
        registerFixtureTools();
        m_postedMessages.clear();
        m_reloadCalls = 0;
    }

    void cleanup() {
        m_handler.reset();
        m_toolSvc.reset();
    }

    void test_nonSlashInput_returnsFalse() {
        const bool handled = m_handler->tryHandle(baseContext(QStringLiteral("hello world")));
        QCOMPARE(handled, false);
        QCOMPARE(m_postedMessages.size(), 0);
        QCOMPARE(m_reloadCalls, 0);
    }

    void test_unknownCommand_returnsFalse() {
        const bool handled =
            m_handler->tryHandle(baseContext(QStringLiteral("/notacommand stuff")));
        QCOMPARE(handled, false);
        QCOMPARE(m_postedMessages.size(), 0);
        QCOMPARE(m_reloadCalls, 0);
    }

    void test_help_postsSystemMessage() {
        const bool handled = m_handler->tryHandle(baseContext(QStringLiteral("/help")));
        QCOMPARE(handled, true);
        QCOMPARE(m_postedMessages.size(), 1);
        QVERIFY(m_postedMessages.first().contains(QStringLiteral("## Available Commands")));
        QVERIFY(m_postedMessages.first().contains(QStringLiteral("/clear")));
        QCOMPARE(m_reloadCalls, 0);
    }

    void test_clear_triggersReloadCallback() {
        const bool handled = m_handler->tryHandle(baseContext(QStringLiteral("/clear")));
        QCOMPARE(handled, true);
        QCOMPARE(m_reloadCalls, 1);
        QCOMPARE(m_postedMessages.size(), 0);
    }

    void test_showtools_noFilter_postsSummary() {
        const bool handled = m_handler->tryHandle(baseContext(QStringLiteral("/showtools")));
        QCOMPARE(handled, true);
        QCOMPARE(m_postedMessages.size(), 1);
        const QString out = m_postedMessages.first();
        QVERIFY(out.contains(QStringLiteral("## Tools Summary")));
        QVERIFY(out.contains(QStringLiteral("Built-in:")));
        QVERIFY(out.contains(QStringLiteral("Custom:")));
        QVERIFY(out.contains(QStringLiteral("MCP:")));
        QVERIFY(out.contains(QStringLiteral("Total:")));
    }

    void test_showtools_filterBuiltin_listsBuiltins() {
        const bool handled =
            m_handler->tryHandle(baseContext(QStringLiteral("/showtools builtin")));
        QCOMPARE(handled, true);
        QCOMPARE(m_postedMessages.size(), 1);
        const QString out = m_postedMessages.first();
        QVERIFY(out.contains(QStringLiteral("## Built-in Tools")));
        QVERIFY(out.contains(QStringLiteral("run_shell")));
        QVERIFY(out.contains(QStringLiteral("read_file")));
        QVERIFY(!out.contains(QStringLiteral("my_custom_tool")));
        QVERIFY(!out.contains(QStringLiteral("slackmcp:")));
    }

    void test_showtools_filterCustom_listsCustom() {
        const bool handled = m_handler->tryHandle(baseContext(QStringLiteral("/showtools custom")));
        QCOMPARE(handled, true);
        const QString out = m_postedMessages.first();
        QVERIFY(out.contains(QStringLiteral("## Custom Tools")));
        QVERIFY(out.contains(QStringLiteral("my_custom_tool")));
        QVERIFY(!out.contains(QStringLiteral("run_shell")));
    }

    void test_showtools_filterMcp_listsMcpByServer() {
        const bool handled = m_handler->tryHandle(baseContext(QStringLiteral("/showtools mcp")));
        QCOMPARE(handled, true);
        const QString out = m_postedMessages.first();
        QVERIFY(out.contains(QStringLiteral("## MCP Tools")));
        QVERIFY(out.contains(QStringLiteral("### slackmcp")));
        QVERIFY(out.contains(QStringLiteral("send_message")));
        QVERIFY(out.contains(QStringLiteral("list_channels")));
    }

    void test_showtools_filterServerName_listsOneServer() {
        const bool handled =
            m_handler->tryHandle(baseContext(QStringLiteral("/showtools slackmcp")));
        QCOMPARE(handled, true);
        const QString out = m_postedMessages.first();
        QVERIFY(out.contains(QStringLiteral("## MCP Server: slackmcp")));
        QVERIFY(out.contains(QStringLiteral("send_message")));
        QVERIFY(out.contains(QStringLiteral("list_channels")));
    }

    void test_showtools_fuzzyMatch_listsMatches() {
        const bool handled = m_handler->tryHandle(baseContext(QStringLiteral("/showtools file")));
        QCOMPARE(handled, true);
        const QString out = m_postedMessages.first();
        QVERIFY(out.contains(QStringLiteral("## Tools matching \"file\"")));
        QVERIFY(out.contains(QStringLiteral("read_file")));
    }

    void test_showmcptools_alias_equalsShowtoolsMcp() {
        m_handler->tryHandle(baseContext(QStringLiteral("/showtools mcp")));
        const QString viaFilter = m_postedMessages.first();
        m_postedMessages.clear();

        m_handler->tryHandle(baseContext(QStringLiteral("/showmcptools")));
        const QString viaAlias = m_postedMessages.first();
        QCOMPARE(viaAlias, viaFilter);
    }

    void test_artifacts_withProject_mentionsProjectName() {
        auto ctx = baseContext(QStringLiteral("/artifacts"));
        ctx.resolveArtifactsLocation = []() {
            SlashCommandContext::ArtifactsLocation loc;
            loc.path = QStringLiteral("/home/user/artifacts/myproj");
            loc.projectName = QStringLiteral("MyProject");
            return loc;
        };
        const bool handled = m_handler->tryHandle(ctx);
        QCOMPARE(handled, true);
        const QString out = m_postedMessages.first();
        QVERIFY(out.contains(QStringLiteral("## Artifacts Directory")));
        QVERIFY(out.contains(QStringLiteral("**MyProject**")));
        QVERIFY(out.contains(QStringLiteral("/home/user/artifacts/myproj")));
    }

    void test_artifacts_withoutProject_mentionsPerConversation() {
        auto ctx = baseContext(QStringLiteral("/artifacts"));
        ctx.resolveArtifactsLocation = []() {
            SlashCommandContext::ArtifactsLocation loc;
            loc.path = QStringLiteral("/home/user/artifacts/conv-abc");
            loc.projectName = QString();
            return loc;
        };
        const bool handled = m_handler->tryHandle(ctx);
        QCOMPARE(handled, true);
        const QString out = m_postedMessages.first();
        QVERIFY(out.contains(QStringLiteral("per-conversation directory")));
        QVERIFY(out.contains(QStringLiteral("/home/user/artifacts/conv-abc")));
        QVERIFY(!out.contains(QStringLiteral("project **")));
    }

    void test_availableCommands_listsKnownAndStaysConsistent() {
        const QVariantList cmds = m_handler->availableCommands();
        QVERIFY(!cmds.isEmpty());

        QStringList names;
        for (const QVariant& v : cmds) {
            const QVariantMap m = v.toMap();
            const QString name = m.value(QStringLiteral("name")).toString();
            const QString summary = m.value(QStringLiteral("summary")).toString();
            const QString usage = m.value(QStringLiteral("usage")).toString();
            QVERIFY2(name.startsWith(QLatin1Char('/')),
                     qPrintable(QStringLiteral("command name missing slash: %1").arg(name)));
            QVERIFY2(!summary.isEmpty(),
                     qPrintable(QStringLiteral("empty summary for %1").arg(name)));
            QVERIFY2(!usage.isEmpty(), qPrintable(QStringLiteral("empty usage for %1").arg(name)));
            names.append(name);
        }

        for (const QString& expected : {QStringLiteral("/showtools"),
                                        QStringLiteral("/showmcptools"),
                                        QStringLiteral("/artifacts"),
                                        QStringLiteral("/compact"),
                                        QStringLiteral("/flashmemory"),
                                        QStringLiteral("/clear"),
                                        QStringLiteral("/help")}) {
            QVERIFY2(names.contains(expected),
                     qPrintable(QStringLiteral("catalogue missing %1").arg(expected)));
        }

        for (const QString& name : names) {
            m_postedMessages.clear();
            m_reloadCalls = 0;
            const bool handled = m_handler->tryHandle(baseContext(name));
            QVERIFY2(
                handled,
                qPrintable(
                    QStringLiteral("catalogue command not handled by tryHandle: %1").arg(name)));
        }
    }
};

QTEST_APPLESS_MAIN(TestSlashCommandService)
#include "test-slash-command-service.moc"
