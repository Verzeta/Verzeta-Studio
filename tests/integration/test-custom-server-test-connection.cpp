// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/db-manager.h"
#include "services/custom-server-registry.h"
#include "services/model-router.h"
#include "services/settings-service.h"

#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <QByteArray>
#include <QDateTime>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QStandardPaths>


class MockHttpServer : public QObject {
    Q_OBJECT
  public:
    explicit MockHttpServer(QObject* parent = nullptr) : QObject(parent) {
        connect(&m_server, &QTcpServer::newConnection, this, &MockHttpServer::onNewConnection);
    }

    bool listen() { return m_server.listen(QHostAddress::LocalHost, 0); }

    quint16 port() const { return m_server.serverPort(); }

    void setResponse(const QByteArray& response) { m_response = response; }

  private slots:
    void onNewConnection() {
        QTcpSocket* socket = m_server.nextPendingConnection();
        if (!socket) {
            return;
        }
        auto* responded = new bool(false);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket, responded]() {
            socket->readAll();
            if (*responded) {
                return;
            }
            *responded = true;
            socket->write(m_response);
            socket->flush();
            QTimer::singleShot(300, socket, &QTcpSocket::disconnectFromHost);
        });
        connect(socket, &QTcpSocket::disconnected, socket, [socket, responded]() {
            delete responded;
            socket->deleteLater();
        });
    }

  private:
    QTcpServer m_server;
    QByteArray m_response;
};


static QByteArray
buildResponse(int statusCode, const QByteArray& body, const QByteArray& reasonPhrase = "OK") {
    QByteArray r;
    r += "HTTP/1.1 " + QByteArray::number(statusCode) + " " + reasonPhrase + "\r\n";
    r += "Content-Type: application/json\r\n";
    r += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
    r += "Connection: close\r\n";
    r += "\r\n";
    r += body;
    return r;
}

struct Verdict {
    QString correlationToken;
    bool ok = false;
    QString verdictKey;
    QString humanMessage;
    QStringList models;
};


class TestCustomServerTestConnection : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    QString m_dbPath;
    std::unique_ptr<SettingsService> m_settings;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<CustomServerRegistry> m_registry;
    std::unique_ptr<MockHttpServer> m_server;

  private slots:

    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void cleanupTestCase() { QStandardPaths::setTestModeEnabled(false); }

    void init() {
        QVERIFY(m_tempDir.isValid());
        m_dbPath =
            m_tempDir.path() + QStringLiteral("/tc_%1.db").arg(QDateTime::currentMSecsSinceEpoch());

        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(m_dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_settings = std::make_unique<SettingsService>(DbManager::instance());
        m_router = std::make_unique<ModelRouter>();
        m_registry = std::make_unique<CustomServerRegistry>(*m_router, *m_settings);

        m_server = std::make_unique<MockHttpServer>();
        QVERIFY(m_server->listen());
    }

    void cleanup() {
        m_server.reset();
        m_registry.reset();
        m_router.reset();
        m_settings.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    }

    QString baseUrl() const {
        return QStringLiteral("http://127.0.0.1:%1/v1").arg(m_server->port());
    }

    Verdict runTest(const QString& base,
                    bool requiresKey,
                    const QString& apiKey,
                    const QString& token = QStringLiteral("test-token")) {
        QSignalSpy spy(m_registry.get(), &CustomServerRegistry::connectionTestResult);
        m_registry->testConnection(token, base, requiresKey, apiKey);
        if (spy.count() == 0) {
            if (!spy.wait(5000)) {
                qWarning("connectionTestResult signal never fired");
                return Verdict{};
            }
        }
        const QList<QVariant> args = spy.takeFirst();
        return Verdict{
            args.at(0).toString(),
            args.at(1).toBool(),
            args.at(2).toString(),
            args.at(3).toString(),
            args.at(4).toStringList(),
        };
    }

    void test_verdict_unreachable() {
        const Verdict v = runTest(QStringLiteral("http://127.0.0.1:9/v1"), false, QString());
        QCOMPARE(v.verdictKey, QStringLiteral("unreachable"));
        QVERIFY(!v.ok);
        QVERIFY(!v.humanMessage.isEmpty());
        QVERIFY(v.models.isEmpty());
        QCOMPARE(v.correlationToken, QStringLiteral("test-token"));
    }

    void test_verdict_auth_rejected() {
        m_server->setResponse(
            buildResponse(401, "{\"error\":\"invalid_api_key\"}", "Unauthorized"));
        const Verdict v = runTest(baseUrl(), false, QStringLiteral("sk-bad-key"));
        QCOMPARE(v.verdictKey, QStringLiteral("auth_rejected"));
        QVERIFY(!v.ok);
    }

    void test_verdict_auth_required() {
        m_server->setResponse(
            buildResponse(401, "{\"error\":\"missing_api_key\"}", "Unauthorized"));
        const Verdict v = runTest(baseUrl(), false, QString());
        QCOMPARE(v.verdictKey, QStringLiteral("auth_required"));
        QVERIFY(!v.ok);
    }

    void test_verdict_path_not_found() {
        m_server->setResponse(buildResponse(404, "<html>404</html>", "Not Found"));
        const Verdict v = runTest(baseUrl(), false, QString());
        QCOMPARE(v.verdictKey, QStringLiteral("path_not_found"));
        QVERIFY(!v.ok);
        QVERIFY(v.humanMessage.contains(QStringLiteral("/v1")));
    }

    void test_verdict_server_error() {
        m_server->setResponse(
            buildResponse(503, "{\"error\":\"out_of_memory\"}", "Service Unavailable"));
        const Verdict v = runTest(baseUrl(), false, QString());
        QCOMPARE(v.verdictKey, QStringLiteral("server_error"));
        QVERIFY(!v.ok);
        QVERIFY(v.humanMessage.contains(QStringLiteral("503")));
    }

    void test_verdict_no_models_loaded() {
        m_server->setResponse(buildResponse(200, "{\"object\":\"list\",\"data\":[]}"));
        const Verdict v = runTest(baseUrl(), false, QString());
        QCOMPARE(v.verdictKey, QStringLiteral("no_models_loaded"));
        QVERIFY(!v.ok);
        QVERIFY(v.models.isEmpty());
    }

    void test_verdict_ok_populates_models() {
        const QByteArray body = "{\"object\":\"list\",\"data\":["
                                "  {\"id\":\"llama-3.1-8b-instruct\",\"object\":\"model\"},"
                                "  {\"id\":\"qwen2.5-coder-7b\",\"object\":\"model\"},"
                                "  {\"id\":\"phi-3-mini\",\"object\":\"model\"}"
                                "]}";
        m_server->setResponse(buildResponse(200, body));
        const Verdict v = runTest(baseUrl(), false, QString());
        QCOMPARE(v.verdictKey, QStringLiteral("ok"));
        QVERIFY(v.ok);
        QCOMPARE(v.models.size(), 3);
        QVERIFY(v.models.contains(QStringLiteral("llama-3.1-8b-instruct")));
        QVERIFY(v.models.contains(QStringLiteral("qwen2.5-coder-7b")));
        QVERIFY(v.models.contains(QStringLiteral("phi-3-mini")));
        QVERIFY(v.humanMessage.contains(QStringLiteral("3 model")));
    }

    void test_correlation_token_roundtrips() {
        const QString token = QStringLiteral("setup-sheet-instance-A");
        const Verdict v = runTest(QStringLiteral("http://127.0.0.1:9/v1"), false, QString(), token);
        QCOMPARE(v.correlationToken, token);
    }
};

QTEST_MAIN(TestCustomServerTestConnection)
#include "test-custom-server-test-connection.moc"
