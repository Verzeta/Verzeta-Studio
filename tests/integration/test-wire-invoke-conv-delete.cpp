// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/db-manager.h"
#include "remote/wire-host-bridge.h"
#include "remote/wire-protocol.h"
#include "services/conversation-controller.h"
#include "services/conversation-service.h"
#include "services/model-router.h"

#include <QTemporaryDir>
#include <QtTest>

#include <memory>
#include <QCoreApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QSqlDatabase>
#include <QUuid>

using Verzeta::Remote::WireHostBridge;
namespace IpcType = Verzeta::Remote::IpcType;
namespace Targets = Verzeta::Remote::Targets;

namespace {

template <typename Predicate> bool spinUntil(Predicate predicate, int timeoutMs) {
    const qint64 deadline = QDateTime::currentMSecsSinceEpoch() + timeoutMs;
    while (QDateTime::currentMSecsSinceEpoch() < deadline) {
        if (predicate())
            return true;
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
    }
    return predicate();
}

QByteArray frameOf(const QJsonObject& obj) {
    const QByteArray payload = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    QByteArray out;
    out.reserve(4 + payload.size());
    const quint32 len = static_cast<quint32>(payload.size());
    out.append(static_cast<char>((len >> 24) & 0xff));
    out.append(static_cast<char>((len >> 16) & 0xff));
    out.append(static_cast<char>((len >> 8) & 0xff));
    out.append(static_cast<char>(len & 0xff));
    out.append(payload);
    return out;
}

}  // namespace

class TestWireInvokeConvDelete : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    QString m_dbPath;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<ConversationController> m_convCtrl;
    std::unique_ptr<WireHostBridge> m_bridge;
    std::unique_ptr<QLocalSocket> m_daemonSocket;

  private slots:
    void initTestCase() {
        qputenv("VERZETA_BRIDGE_SOCKET",
                QByteArrayLiteral("verzeta-host-bridge-test-del-") +
                    QByteArray::number(QCoreApplication::applicationPid()));
    }

    void init() {
        QVERIFY(m_tempDir.isValid());
        m_dbPath = m_tempDir.path() +
                   QStringLiteral("/test_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
        DbManager::instance().close();
        QFile::remove(m_dbPath);
        QVERIFY(DbManager::instance().open(m_dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_convSvc = std::make_unique<ConversationService>(DbManager::instance(), nullptr);
        m_router = std::make_unique<ModelRouter>(nullptr);
        m_convCtrl = std::make_unique<ConversationController>(*m_convSvc, *m_router);

        WireHostBridge::Services svc;
        svc.convCtrl = QPointer<ConversationController>(m_convCtrl.get());
        svc.convSvc = QPointer<ConversationService>(m_convSvc.get());
        m_bridge = std::make_unique<WireHostBridge>(svc);

        m_daemonSocket = std::make_unique<QLocalSocket>();
        const QString name = Verzeta::Remote::hostBridgeSocketName();
        QVERIFY2(spinUntil(
                     [this, &name] {
                         if (m_daemonSocket->state() == QLocalSocket::ConnectedState)
                             return true;
                         if (m_daemonSocket->state() == QLocalSocket::UnconnectedState) {
                             m_daemonSocket->connectToServer(name);
                         }
                         return m_daemonSocket->state() == QLocalSocket::ConnectedState;
                     },
                     3000),
                 "stub daemon failed to connect to bridge QLocalServer");
    }

    void cleanup() {
        if (m_daemonSocket) {
            m_daemonSocket->disconnectFromServer();
            m_daemonSocket.reset();
        }
        m_bridge.reset();
        m_convCtrl.reset();
        m_router.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QFile::remove(m_dbPath);
    }

    void invokeConvDelete_deletesTheRow() {
        const QString convId = m_convSvc->createConversation(QStringLiteral("doomed"));
        QVERIFY(!convId.isEmpty());
        QVERIFY(m_convSvc->getConversation(convId).has_value());

        const QJsonObject frame{
            {QStringLiteral("type"), QString::fromLatin1(IpcType::Invoke)},
            {QStringLiteral("target"), QString::fromLatin1(Targets::Conversations)},
            {QStringLiteral("method"), QStringLiteral("deleteConversation")},
            {QStringLiteral("args"),
             QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                    {QStringLiteral("value"), convId}}}},
        };
        m_daemonSocket->write(frameOf(frame));
        m_daemonSocket->flush();

        QVERIFY2(spinUntil([&] { return !m_convSvc->getConversation(convId).has_value(); }, 5000),
                 "conversation row still exists after wire-invoked delete — "
                 "the remote delete chain is broken");
    }

    void invokeConvDelete_groupConversation() {
        const QString convId = m_convSvc->createGroupConversation(
            QStringLiteral("doomed group"),
            QStringList{QUuid::createUuid().toString(QUuid::WithoutBraces)});
        QVERIFY(!convId.isEmpty());

        const QJsonObject frame{
            {QStringLiteral("type"), QString::fromLatin1(IpcType::Invoke)},
            {QStringLiteral("target"), QString::fromLatin1(Targets::Conversations)},
            {QStringLiteral("method"), QStringLiteral("deleteConversation")},
            {QStringLiteral("args"),
             QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                    {QStringLiteral("value"), convId}}}},
        };
        m_daemonSocket->write(frameOf(frame));
        m_daemonSocket->flush();

        QVERIFY2(spinUntil([&] { return !m_convSvc->getConversation(convId).has_value(); }, 5000),
                 "group conversation row survived the wire-invoked delete");
    }
};

QTEST_MAIN(TestWireInvokeConvDelete)
#include "test-wire-invoke-conv-delete.moc"
