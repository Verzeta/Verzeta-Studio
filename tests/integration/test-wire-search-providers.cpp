// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/db-manager.h"
#include "remote/wire-host-bridge.h"
#include "remote/wire-protocol.h"
#include "services/search/web-search-provider-registry.h"
#include "services/search/web-search-service.h"
#include "services/settings-service.h"

#include <QTemporaryDir>
#include <QtTest>

#include <memory>
#include <QCoreApplication>
#include <QDateTime>
#include <QEventLoop>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalSocket>
#include <QSqlDatabase>

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

class TestWireSearchProviders : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    QString m_dbPath;
    std::unique_ptr<SettingsService> m_settings;
    std::unique_ptr<Search::WebSearchService> m_search;
    std::unique_ptr<Search::WebSearchProviderRegistry> m_registry;
    std::unique_ptr<WireHostBridge> m_bridge;
    std::unique_ptr<QLocalSocket> m_daemonSocket;

  private slots:
    void initTestCase() {
        qputenv("VERZETA_BRIDGE_SOCKET",
                QByteArrayLiteral("verzeta-host-bridge-test-search-") +
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

        m_settings = std::make_unique<SettingsService>(DbManager::instance());
        m_search = std::make_unique<Search::WebSearchService>();
        m_registry = std::make_unique<Search::WebSearchProviderRegistry>(*m_search, *m_settings);

        WireHostBridge::Services svc;
        svc.settings = QPointer<SettingsService>(m_settings.get());
        svc.webSearchRegistry = QPointer<Search::WebSearchProviderRegistry>(m_registry.get());
        m_bridge = std::make_unique<WireHostBridge>(std::move(svc));

        m_daemonSocket = std::make_unique<QLocalSocket>();
        const QString name = Verzeta::Remote::hostBridgeSocketName();
        QVERIFY2(spinUntil(
                     [this, &name] {
                         if (m_daemonSocket->state() == QLocalSocket::ConnectedState)
                             return true;
                         if (m_daemonSocket->state() == QLocalSocket::UnconnectedState)
                             m_daemonSocket->connectToServer(name);
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
        m_registry.reset();
        m_search.reset();
        m_settings.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QFile::remove(m_dbPath);
    }

    void invokeSearchSetActive_changesActiveProvider() {
        QCOMPARE(m_registry->activeProviderId(), QStringLiteral("ddg-html"));

        const QJsonObject frame{
            {QStringLiteral("type"), QString::fromLatin1(IpcType::Invoke)},
            {QStringLiteral("target"), QString::fromLatin1(Targets::WebSearchProviders)},
            {QStringLiteral("method"), QStringLiteral("setActiveProvider")},
            {QStringLiteral("args"),
             QJsonArray{QJsonObject{{QStringLiteral("type"), QStringLiteral("string")},
                                    {QStringLiteral("value"), QStringLiteral("tavily")}}}},
        };
        m_daemonSocket->write(frameOf(frame));
        m_daemonSocket->flush();

        QVERIFY2(
            spinUntil([&] { return m_registry->activeProviderId() == QStringLiteral("tavily"); },
                      5000),
            "active web-search provider did not change after the "
            "wire-invoked set_active — the remote selection chain is broken");

        QCOMPARE(m_search->activeConfig().providerId, QStringLiteral("tavily"));
    }
};

QTEST_MAIN(TestWireSearchProviders)
#include "test-wire-search-providers.moc"
