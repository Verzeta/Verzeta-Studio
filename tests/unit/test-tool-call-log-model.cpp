// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/db-manager.h"
#include "models/tool-call-log-model.h"
#include "services/conversation-service.h"
#include "services/message-service.h"

#include <QTemporaryDir>
#include <QTest>

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QUuid>

class TestToolCallLogModel : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<MessageService> m_msgSvc;
    std::unique_ptr<ToolCallLogModel> m_model;
    QString m_convId;

    static QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

    QString writeAssistantAnchor(const QString& convId) {
        Message m;
        m.id = uuid();
        m.conversationId = convId;
        m.role = QStringLiteral("assistant");
        m.content = QString{};
        m.finishReason = QStringLiteral("tool_calls");
        m.createdAt = QDateTime::currentDateTimeUtc();
        return m_msgSvc->addMessage(m);
    }

    QString
    addToolCall(const QString& anchorMsgId, const QString& toolName, const QJsonObject& args) {
        ToolCall c;
        c.id = uuid();
        c.messageId = anchorMsgId;
        c.toolName = toolName;
        c.arguments = args;
        c.status = QStringLiteral("running");
        c.startedAt = QDateTime::currentDateTimeUtc();
        return m_msgSvc->addToolCall(c);
    }

  private slots:

    void init() {
        const QString dbPath =
            m_tempDir.path() +
            QStringLiteral("/test_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_convSvc = std::make_unique<ConversationService>(DbManager::instance());
        m_msgSvc = std::make_unique<MessageService>(DbManager::instance());
        m_model = std::make_unique<ToolCallLogModel>(*m_msgSvc);

        m_convId = m_convSvc->createConversation(QStringLiteral("Chat"));
    }

    void cleanup() {
        m_model.reset();
        m_msgSvc.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    }

    void test_initialState_isEmpty() { QCOMPARE(m_model->rowCount(), 0); }

    void test_addToolCall_insertsRowWithArgs() {
        const QString anchor = writeAssistantAnchor(m_convId);
        m_model->setActiveConversation(m_convId);

        QSignalSpy insertedSpy(m_model.get(), &QAbstractItemModel::rowsInserted);
        addToolCall(anchor,
                    QStringLiteral("run_shell"),
                    QJsonObject{{QStringLiteral("cmd"), QStringLiteral("ls")}});

        QCOMPARE(insertedSpy.count(), 1);
        QCOMPARE(m_model->rowCount(), 1);

        const QModelIndex idx = m_model->index(0, 0);
        QCOMPARE(m_model->data(idx, ToolCallLogModel::ToolNameRole).toString(),
                 QStringLiteral("run_shell"));
        QCOMPARE(m_model->data(idx, ToolCallLogModel::StatusRole).toString(),
                 QStringLiteral("running"));
        const QString args = m_model->data(idx, ToolCallLogModel::ArgsRole).toString();
        QVERIFY(args.contains(QStringLiteral("cmd")));
    }

    void test_updateToolCallResult_updatesRowInPlace() {
        const QString anchor = writeAssistantAnchor(m_convId);
        m_model->setActiveConversation(m_convId);
        const QString callId =
            addToolCall(anchor,
                        QStringLiteral("write_file"),
                        QJsonObject{{QStringLiteral("path"), QStringLiteral("/tmp/x")}});

        QSignalSpy dataSpy(m_model.get(), &QAbstractItemModel::dataChanged);
        QVERIFY(m_msgSvc->updateToolCallResult(
            callId, QJsonObject{{QStringLiteral("written"), true}}, QStringLiteral("success")));

        QVERIFY(dataSpy.count() >= 1);
        const QModelIndex idx = m_model->index(0, 0);
        QCOMPARE(m_model->data(idx, ToolCallLogModel::StatusRole).toString(),
                 QStringLiteral("success"));
        const QString result = m_model->data(idx, ToolCallLogModel::ResultRole).toString();
        QVERIFY(result.contains(QStringLiteral("written")));
    }

    void test_hugeResult_truncatedForDisplay() {
        const QString anchor = writeAssistantAnchor(m_convId);
        m_model->setActiveConversation(m_convId);
        const QString callId = addToolCall(anchor, QStringLiteral("list_files"), QJsonObject{});

        QJsonArray big;
        for (int i = 0; i < 5000; ++i) {
            big.append(QStringLiteral("/some/long/path/entry_%1.txt").arg(i));
        }
        QVERIFY(m_msgSvc->updateToolCallResult(
            callId, QJsonObject{{QStringLiteral("files"), big}}, QStringLiteral("success")));

        const QModelIndex idx = m_model->index(0, 0);
        const QString shown = m_model->data(idx, ToolCallLogModel::ResultRole).toString();
        QVERIFY(shown.contains(QStringLiteral("[truncated")));
        QVERIFY(shown.size() < 20000);
    }

    void test_toolCallInOtherConversation_isIgnored() {
        const QString anchorA = writeAssistantAnchor(m_convId);
        m_model->setActiveConversation(m_convId);

        const QString otherConv = m_convSvc->createConversation(QStringLiteral("Other"));
        const QString anchorB = writeAssistantAnchor(otherConv);
        addToolCall(anchorB, QStringLiteral("run_shell"), QJsonObject{});

        QCOMPARE(m_model->rowCount(), 0);

        addToolCall(anchorA, QStringLiteral("write_file"), QJsonObject{});
        QCOMPARE(m_model->rowCount(), 1);
    }

    void test_setActiveConversation_reloadsExistingCalls() {
        const QString anchor = writeAssistantAnchor(m_convId);
        addToolCall(anchor, QStringLiteral("tool_a"), QJsonObject{});
        addToolCall(anchor, QStringLiteral("tool_b"), QJsonObject{});

        QSignalSpy resetSpy(m_model.get(), &QAbstractItemModel::modelReset);
        m_model->setActiveConversation(m_convId);

        QCOMPARE(resetSpy.count(), 1);
        QCOMPARE(m_model->rowCount(), 2);
    }
};

QTEST_MAIN(TestToolCallLogModel)
#include "test-tool-call-log-model.moc"
