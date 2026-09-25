// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/db-manager.h"
#include "models/message.h"
#include "services/conversation-service.h"
#include "services/export-service.h"
#include "services/message-service.h"

#include <QTemporaryDir>
#include <QTest>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QObject>
#include <QStandardPaths>
#include <QUuid>

class TestExportService : public QObject {
    Q_OBJECT

  private:
    DbManager* m_db = nullptr;
    ConversationService* m_convSvc = nullptr;
    MessageService* m_msgSvc = nullptr;
    ExportService* m_exSvc = nullptr;
    QTemporaryDir m_tmpDir;

    QString createConv(const QString& title = QStringLiteral("Test Chat"),
                       const QString& systemPrompt = {}) {
        const QString id = m_convSvc->createConversation(title);
        if (!systemPrompt.isEmpty()) {
            m_convSvc->updateSystemPrompt(id, systemPrompt);
        }
        return id;
    }

    QString addMessage(const QString& convId,
                       const QString& role,
                       const QString& content,
                       int tokenCount = 0,
                       const QString& finishReason = {},
                       const QString& modelUsed = {}) {
        Message msg;
        msg.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        msg.conversationId = convId;
        msg.role = role;
        msg.content = content;
        msg.createdAt = QDateTime::currentDateTimeUtc();
        msg.tokenCount = tokenCount;
        msg.finishReason = finishReason;
        msg.modelUsed = modelUsed;
        return m_msgSvc->addMessage(msg);
    }

  private slots:


    void init() {
        m_db = &DbManager::instance();
        m_db->open(QStringLiteral(":memory:"));
        m_db->runMigrations();

        m_convSvc = new ConversationService(*m_db, this);
        m_msgSvc = new MessageService(*m_db, this);
        m_exSvc = new ExportService(*m_convSvc, *m_msgSvc, this);
    }

    void cleanup() {
        m_db->close();
        delete m_exSvc;
        m_exSvc = nullptr;
        delete m_msgSvc;
        m_msgSvc = nullptr;
        delete m_convSvc;
        m_convSvc = nullptr;
    }


    void testMarkdownFormat() {
        const QString convId = createConv(QStringLiteral("My Conversation"));
        addMessage(convId, "user", "Hello, world!");
        addMessage(convId, "assistant", "Hi there!", 10, "stop", "gpt-4o");

        const QString dest = m_tmpDir.path() + QStringLiteral("/tc01.md");
        QVERIFY(m_exSvc->exportToMarkdown(convId, dest));
        QVERIFY(QFile::exists(dest));

        QFile f(dest);
        QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString content = QString::fromUtf8(f.readAll());

        QVERIFY(content.contains(QStringLiteral("# My Conversation")));
        QVERIFY(content.contains(QStringLiteral("**You**")));
        QVERIFY(content.contains(QStringLiteral("Hello, world!")));
        QVERIFY(content.contains(QStringLiteral("**Assistant**")));
        QVERIFY(content.contains(QStringLiteral("Hi there!")));
        QVERIFY(content.contains(QStringLiteral("gpt-4o")));
        QVERIFY(content.contains(QStringLiteral("Verzeta Studio")));
    }


    void testJsonFormat() {
        const QString convId = createConv(QStringLiteral("JSON Test"));
        addMessage(convId, "user", "Question", 5);
        addMessage(convId, "assistant", "Answer", 15, "stop", "claude-3");

        const QString dest = m_tmpDir.path() + QStringLiteral("/tc02.json");
        QVERIFY(m_exSvc->exportToJson(convId, dest));
        QVERIFY(QFile::exists(dest));

        QFile f(dest);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        QVERIFY(!doc.isNull());
        QVERIFY(doc.isObject());

        const QJsonObject root = doc.object();
        QVERIFY(root.contains(QStringLiteral("version")));
        QCOMPARE(root.value(QStringLiteral("version")).toString(), QStringLiteral("1.0"));
        QVERIFY(root.contains(QStringLiteral("conversation")));
        QVERIFY(root.contains(QStringLiteral("messages")));
        QVERIFY(root.contains(QStringLiteral("statistics")));

        const QJsonArray msgs = root.value(QStringLiteral("messages")).toArray();
        QCOMPARE(msgs.size(), 2);

        const QJsonObject firstMsg = msgs.first().toObject();
        QVERIFY(firstMsg.contains(QStringLiteral("id")));
        QVERIFY(firstMsg.contains(QStringLiteral("role")));
        QVERIFY(firstMsg.contains(QStringLiteral("content")));
        QVERIFY(firstMsg.contains(QStringLiteral("created_at")));
        QVERIFY(firstMsg.contains(QStringLiteral("token_count")));
        QVERIFY(firstMsg.contains(QStringLiteral("tool_calls")));
        QVERIFY(firstMsg.contains(QStringLiteral("attachments")));
    }


    void testEmptyConversation() {
        const QString convId = createConv(QStringLiteral("Empty"));

        const QString mdDest = m_tmpDir.path() + QStringLiteral("/tc03.md");
        const QString jsonDest = m_tmpDir.path() + QStringLiteral("/tc03.json");

        QVERIFY(m_exSvc->exportToMarkdown(convId, mdDest));
        QVERIFY(m_exSvc->exportToJson(convId, jsonDest));

        QFile mdFile(mdDest);
        QVERIFY(mdFile.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString mdContent = QString::fromUtf8(mdFile.readAll());
        QVERIFY(mdContent.contains(QStringLiteral("# Empty")));

        QFile jsonFile(jsonDest);
        QVERIFY(jsonFile.open(QIODevice::ReadOnly));
        const QJsonDocument doc = QJsonDocument::fromJson(jsonFile.readAll());
        QVERIFY(!doc.isNull());
        const QJsonArray msgs = doc.object().value(QStringLiteral("messages")).toArray();
        QCOMPARE(msgs.size(), 0);
    }


    void testCodeBlockPreservation() {
        const QString convId = createConv(QStringLiteral("Code Test"));
        const QString codeMsg =
            QStringLiteral("Here is some code:\n\n```python\nprint('hello')\n```\n\nDone.");
        addMessage(convId, "assistant", codeMsg, 20, "stop");

        const QString dest = m_tmpDir.path() + QStringLiteral("/tc04.md");
        QVERIFY(m_exSvc->exportToMarkdown(convId, dest));

        QFile f(dest);
        QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString content = QString::fromUtf8(f.readAll());

        QVERIFY(content.contains(QStringLiteral("```python")));
        QVERIFY(content.contains(QStringLiteral("print('hello')")));
        QVERIFY(content.contains(QStringLiteral("```")));
    }


    void testSystemPromptIncluded() {
        const QString convId = createConv(QStringLiteral("SysPrompt Test"),
                                          QStringLiteral("You are a helpful coding assistant."));
        addMessage(convId, "user", "Hello");

        const QString mdDest = m_tmpDir.path() + QStringLiteral("/tc05.md");
        const QString jsonDest = m_tmpDir.path() + QStringLiteral("/tc05.json");

        QVERIFY(m_exSvc->exportToMarkdown(convId, mdDest));
        QVERIFY(m_exSvc->exportToJson(convId, jsonDest));

        QFile mdFile(mdDest);
        QVERIFY(mdFile.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString mdContent = QString::fromUtf8(mdFile.readAll());
        QVERIFY(mdContent.contains(QStringLiteral("> You are a helpful coding assistant.")));

        QFile jsonFile(jsonDest);
        QVERIFY(jsonFile.open(QIODevice::ReadOnly));
        const QJsonDocument doc = QJsonDocument::fromJson(jsonFile.readAll());
        const QString systemPrompt = doc.object()
                                         .value(QStringLiteral("conversation"))
                                         .toObject()
                                         .value(QStringLiteral("system_prompt"))
                                         .toString();
        QCOMPARE(systemPrompt, QStringLiteral("You are a helpful coding assistant."));
    }


    void testToolCallsExported() {
        const QString convId = createConv(QStringLiteral("Tool Test"));
        const QString msgId = addMessage(
            convId, "assistant", "I'll run the shell command for you.", 10, "tool_calls");

        ToolCall tc;
        tc.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        tc.messageId = msgId;
        tc.toolName = QStringLiteral("run_shell");
        tc.arguments = QJsonObject{{QStringLiteral("command"), QStringLiteral("ls -la")}};
        tc.result = QJsonValue(QStringLiteral("total 8\ndrwxr-xr-x 2 user user 4096"));
        tc.status = QStringLiteral("success");
        tc.startedAt = QDateTime::currentDateTimeUtc();
        tc.completedAt = QDateTime::currentDateTimeUtc();
        m_msgSvc->addToolCall(tc);

        const QString mdDest = m_tmpDir.path() + QStringLiteral("/tc06.md");
        const QString jsonDest = m_tmpDir.path() + QStringLiteral("/tc06.json");

        QVERIFY(m_exSvc->exportToMarkdown(convId, mdDest));
        QVERIFY(m_exSvc->exportToJson(convId, jsonDest));

        QFile mdFile(mdDest);
        QVERIFY(mdFile.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString mdContent = QString::fromUtf8(mdFile.readAll());
        QVERIFY(mdContent.contains(QStringLiteral("run_shell")));
        QVERIFY(mdContent.contains(QStringLiteral("ls -la")));

        QFile jsonFile(jsonDest);
        QVERIFY(jsonFile.open(QIODevice::ReadOnly));
        const QJsonDocument doc = QJsonDocument::fromJson(jsonFile.readAll());
        const QJsonArray msgs = doc.object().value(QStringLiteral("messages")).toArray();
        bool found = false;
        for (const QJsonValue& v : msgs) {
            const QJsonObject obj = v.toObject();
            if (obj.value(QStringLiteral("role")).toString() == QStringLiteral("assistant")) {
                const QJsonArray toolCalls = obj.value(QStringLiteral("tool_calls")).toArray();
                QVERIFY(toolCalls.size() > 0);
                QCOMPARE(toolCalls.first().toObject().value(QStringLiteral("tool_name")).toString(),
                         QStringLiteral("run_shell"));
                found = true;
            }
        }
        QVERIFY(found);
    }


    void testAttachmentReferences() {
        const QString convId = createConv(QStringLiteral("Attachment Test"));
        const QString msgId = addMessage(convId, "user", "See the attached diagram.", 5);

        Attachment att;
        att.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        att.messageId = msgId;
        att.type = QStringLiteral("image");
        att.filename = QStringLiteral("diagram.png");
        att.mimeType = QStringLiteral("image/png");
        att.createdAt = QDateTime::currentDateTimeUtc();
        m_msgSvc->addAttachment(att);

        const QString mdDest = m_tmpDir.path() + QStringLiteral("/tc07.md");
        const QString jsonDest = m_tmpDir.path() + QStringLiteral("/tc07.json");

        QVERIFY(m_exSvc->exportToMarkdown(convId, mdDest));
        QVERIFY(m_exSvc->exportToJson(convId, jsonDest));

        QFile mdFile(mdDest);
        QVERIFY(mdFile.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString mdContent = QString::fromUtf8(mdFile.readAll());
        QVERIFY(mdContent.contains(QStringLiteral("diagram.png")));

        QFile jsonFile(jsonDest);
        QVERIFY(jsonFile.open(QIODevice::ReadOnly));
        const QJsonDocument doc = QJsonDocument::fromJson(jsonFile.readAll());
        const QJsonArray msgs = doc.object().value(QStringLiteral("messages")).toArray();
        bool found = false;
        for (const QJsonValue& v : msgs) {
            const QJsonObject obj = v.toObject();
            if (obj.value(QStringLiteral("role")).toString() == QStringLiteral("user")) {
                const QJsonArray atts = obj.value(QStringLiteral("attachments")).toArray();
                QVERIFY(atts.size() > 0);
                QCOMPARE(atts.first().toObject().value(QStringLiteral("file_name")).toString(),
                         QStringLiteral("diagram.png"));
                found = true;
            }
        }
        QVERIFY(found);
    }


    void testInterruptedMessageMarker() {
        const QString convId = createConv(QStringLiteral("Interrupted"));
        addMessage(convId, "user", "Tell me something long.", 5);
        addMessage(convId, "assistant", "Once upon a time…", 10, "user_interrupted", "llama3");

        const QString dest = m_tmpDir.path() + QStringLiteral("/tc08.md");
        QVERIFY(m_exSvc->exportToMarkdown(convId, dest));

        QFile f(dest);
        QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString content = QString::fromUtf8(f.readAll());
        QVERIFY(content.contains(QStringLiteral("[INTERRUPTED]")));
    }


    void testTokenStatistics() {
        const QString convId = createConv(QStringLiteral("Token Stats"));
        addMessage(convId, "user", "Prompt", 12);
        addMessage(convId, "assistant", "Answer", 88, "stop");

        const QString dest = m_tmpDir.path() + QStringLiteral("/tc09.json");
        QVERIFY(m_exSvc->exportToJson(convId, dest));

        QFile f(dest);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        const int totalTokens = doc.object()
                                    .value(QStringLiteral("statistics"))
                                    .toObject()
                                    .value(QStringLiteral("total_tokens"))
                                    .toInt();
        QCOMPARE(totalTokens, 100);
    }


    void testUnicodeContent() {
        const QString convId = createConv(QStringLiteral("Unicode 😀 테스트"));
        const QString unicodeMsg = QStringLiteral("Hello 😀! こんにちは. 你好. Привет. مرحبا.");
        addMessage(convId, "user", unicodeMsg, 30);
        addMessage(convId,
                   "assistant",
                   QStringLiteral("Unicode response: ❤️ 日本語テスト"),
                   20,
                   "stop");

        const QString mdDest = m_tmpDir.path() + QStringLiteral("/tc10.md");
        const QString jsonDest = m_tmpDir.path() + QStringLiteral("/tc10.json");

        QVERIFY(m_exSvc->exportToMarkdown(convId, mdDest));
        QVERIFY(m_exSvc->exportToJson(convId, jsonDest));

        QFile mdFile(mdDest);
        QVERIFY(mdFile.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString mdContent = QString::fromUtf8(mdFile.readAll());
        QVERIFY(mdContent.contains(QStringLiteral("😀")));
        QVERIFY(mdContent.contains(QStringLiteral("こんにちは")));
        QVERIFY(mdContent.contains(QStringLiteral("你好")));

        QFile jsonFile(jsonDest);
        QVERIFY(jsonFile.open(QIODevice::ReadOnly));
        const QJsonDocument doc = QJsonDocument::fromJson(jsonFile.readAll());
        const QJsonArray msgs = doc.object().value(QStringLiteral("messages")).toArray();
        const QString firstContent =
            msgs.first().toObject().value(QStringLiteral("content")).toString();
        QVERIFY(firstContent.contains(QStringLiteral("😀")));
        QVERIFY(firstContent.contains(QStringLiteral("こんにちは")));
    }
};

QTEST_MAIN(TestExportService)
#include "test-export-service.moc"
