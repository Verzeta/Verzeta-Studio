// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/db-manager.h"
#include "services/conversation-service.h"
#include "tools/memory/list-project-conversations-tool.h"

#include <QTemporaryDir>
#include <QtTest/QtTest>

class TestListProjectConversationsTool : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    ConversationService* m_convSvc = nullptr;

  private slots:
    void init() {
        QVERIFY(m_tempDir.isValid());
        DbManager::instance().close();
        const QString dbPath = m_tempDir.filePath(QStringLiteral("lpct.db"));
        QVERIFY(DbManager::instance().open(dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_convSvc = new ConversationService(DbManager::instance(), this);
    }

    void cleanup() {
        delete m_convSvc;
        m_convSvc = nullptr;
        DbManager::instance().close();
    }

    void test_contract() {
        Tools::ListProjectConversationsTool tool(*m_convSvc, []() { return QString(); });
        QCOMPARE(tool.name(), QStringLiteral("list_project_conversations"));
        QCOMPARE(tool.runsOnMainThread(), true);
        QVERIFY(tool.parameters().isEmpty());
    }

    void test_invoke_noActive_returnsNoActiveConversationError() {
        Tools::ListProjectConversationsTool tool(*m_convSvc, []() { return QString(); });
        const QJsonObject obj = tool.invoke({}).toObject();
        QVERIFY(obj.contains(QStringLiteral("error")));
        QCOMPARE(obj[QStringLiteral("error")].toString(), QStringLiteral("no active conversation"));
    }

    void test_invoke_noProjectFolder_returnsError() {
        const QString convId = m_convSvc->createConversation(QStringLiteral("plain-standalone"));
        QVERIFY(!convId.isEmpty());

        Tools::ListProjectConversationsTool tool(*m_convSvc, [convId]() { return convId; });
        const QJsonObject obj = tool.invoke({}).toObject();
        QVERIFY(obj.contains(QStringLiteral("error")));
        QCOMPARE(obj[QStringLiteral("error")].toString(),
                 QStringLiteral("current chat is not inside a project"));
    }
};

QTEST_MAIN(TestListProjectConversationsTool)
#include "test-list-project-conversations-tool.moc"
