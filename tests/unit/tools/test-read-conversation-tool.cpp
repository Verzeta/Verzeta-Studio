// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/db-manager.h"
#include "models/message.h"
#include "services/conversation-service.h"
#include "services/message-service.h"
#include "tools/memory/read-conversation-tool.h"

#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <QUuid>

class TestReadConversationTool : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    ConversationService* m_convSvc = nullptr;
    MessageService* m_msgSvc = nullptr;
    QString m_activeConvId;

    static QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

  private slots:
    void init() {
        QVERIFY(m_tempDir.isValid());
        DbManager::instance().close();
        const QString dbPath = m_tempDir.filePath(QStringLiteral("rct.db"));
        QVERIFY(DbManager::instance().open(dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_convSvc = new ConversationService(DbManager::instance(), this);
        m_msgSvc = new MessageService(DbManager::instance(), this);

        m_activeConvId = m_convSvc->createConversation(QStringLiteral("rct-current"));
        QVERIFY(!m_activeConvId.isEmpty());

        Message m;
        m.id = uuid();
        m.conversationId = m_activeConvId;
        m.role = QStringLiteral("user");
        m.content = QStringLiteral("hello from rct");
        m.createdAt = QDateTime::currentDateTimeUtc();
        m_msgSvc->addMessage(m);
    }

    void cleanup() {
        delete m_msgSvc;
        m_msgSvc = nullptr;
        delete m_convSvc;
        m_convSvc = nullptr;
        DbManager::instance().close();
    }

    void test_contract() {
        Tools::ReadConversationTool tool(*m_msgSvc, []() { return QString(); });
        QCOMPARE(tool.name(), QStringLiteral("read_conversation"));
        QCOMPARE(tool.runsOnMainThread(), true);
    }

    void test_parameters() {
        Tools::ReadConversationTool tool(*m_msgSvc, []() { return QString(); });
        const auto params = tool.parameters();
        QCOMPARE(params.size(), 2);
        QCOMPARE(params[0].name, QStringLiteral("conversation_id"));
        QCOMPARE(params[0].required, true);
        QCOMPARE(params[1].name, QStringLiteral("limit"));
        QCOMPARE(params[1].required, false);
    }

    void test_invoke_emptyGetterAndEmptyArg_returnsError() {
        Tools::ReadConversationTool tool(*m_msgSvc, []() { return QString(); });
        QJsonObject args;
        args[QStringLiteral("conversation_id")] = QString();
        QVERIFY(tool.invoke(args).toObject().contains(QStringLiteral("error")));
    }

    void test_invoke_currentKeyword_resolvesViaGetter() {
        Tools::ReadConversationTool tool(*m_msgSvc, [this]() { return m_activeConvId; });
        QJsonObject args;
        args[QStringLiteral("conversation_id")] = QStringLiteral("current");
        const QJsonObject obj = tool.invoke(args).toObject();
        QCOMPARE(obj[QStringLiteral("conversationId")].toString(), m_activeConvId);
        QVERIFY(obj[QStringLiteral("count")].toInt() >= 1);
    }

    void test_invoke_explicitId_bypassesGetter() {
        const QString otherId = m_convSvc->createConversation(QStringLiteral("rct-other"));
        QVERIFY(!otherId.isEmpty());
        Message m;
        m.id = uuid();
        m.conversationId = otherId;
        m.role = QStringLiteral("assistant");
        m.content = QStringLiteral("payload");
        m.createdAt = QDateTime::currentDateTimeUtc();
        m_msgSvc->addMessage(m);

        Tools::ReadConversationTool tool(*m_msgSvc, [this]() { return m_activeConvId; });
        QJsonObject args;
        args[QStringLiteral("conversation_id")] = otherId;
        const QJsonObject obj = tool.invoke(args).toObject();
        QCOMPARE(obj[QStringLiteral("conversationId")].toString(), otherId);
    }
};

QTEST_MAIN(TestReadConversationTool)
#include "test-read-conversation-tool.moc"
