// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "services/model-download-service.h"

#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <QCryptographicHash>
#include <QSignalSpy>

namespace {

class OneShotHttpServer : public QObject {
    Q_OBJECT
  public:
    explicit OneShotHttpServer(QByteArray body, QObject* parent = nullptr)
        : QObject(parent), m_body(std::move(body)) {
        connect(&m_server, &QTcpServer::newConnection, this, [this]() {
            QTcpSocket* sock = m_server.nextPendingConnection();
            connect(sock, &QTcpSocket::readyRead, sock, [this, sock]() {
                if (!sock->readAll().contains("\r\n\r\n"))
                    return;
                QByteArray resp = "HTTP/1.1 200 OK\r\n"
                                  "Content-Type: application/octet-stream\r\n"
                                  "Content-Length: " +
                                  QByteArray::number(m_body.size()) +
                                  "\r\nConnection: close\r\n\r\n" + m_body;
                sock->write(resp);
                sock->flush();
                sock->disconnectFromHost();
            });
        });
        const bool ok = m_server.listen(QHostAddress::LocalHost, 0);
        Q_ASSERT(ok);
    }

    QString url() const {
        return QStringLiteral("http://127.0.0.1:%1/model.gguf").arg(m_server.serverPort());
    }

  private:
    QTcpServer m_server;
    QByteArray m_body;
};

QString sha256Hex(const QByteArray& data) {
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}

}  // namespace

class TestModelDownloadService : public QObject {
    Q_OBJECT

  private slots:
    void test_pinSurface() {
        ModelDownloadService svc;
        QVERIFY(!svc.recommendedFilename(QStringLiteral("ragp")).isEmpty());
        QVERIFY(!svc.recommendedFilename(QStringLiteral("embed")).isEmpty());
        QVERIFY(svc.recommendedBytes(QStringLiteral("ragp")) > 0);
        QVERIFY(svc.recommendedBytes(QStringLiteral("embed")) > 0);
        QVERIFY(svc.recommendedFilename(QStringLiteral("nope")).isEmpty());
        QCOMPARE(svc.recommendedBytes(QStringLiteral("nope")), 0);
        QVERIFY(!svc.start(QStringLiteral("nope")));
    }

    void test_happyPath_downloadVerifyRename() {
        const QByteArray body(64 * 1024, 'x');
        OneShotHttpServer server(body);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());

        ModelDownloadService svc;
        svc.setTargetDirProvider([&dir](const QString&) { return dir.path(); });
        svc.setPinOverrideForTest(QStringLiteral("embed"),
                                  server.url(),
                                  sha256Hex(body),
                                  body.size(),
                                  QStringLiteral("fixture-embed.gguf"));

        QSignalSpy done(&svc, &ModelDownloadService::downloadFinished);
        QSignalSpy failed(&svc, &ModelDownloadService::downloadFailed);
        QVERIFY(!svc.recommendedPresent(QStringLiteral("embed")));
        QVERIFY(svc.start(QStringLiteral("embed")));
        QVERIFY(svc.active());
        QVERIFY(done.wait(10000));
        QCOMPARE(failed.count(), 0);
        QCOMPARE(done.first().at(0).toString(), QStringLiteral("embed"));
        QCOMPARE(done.first().at(1).toString(), QStringLiteral("fixture-embed.gguf"));

        const QString finalPath = dir.path() + QStringLiteral("/fixture-embed.gguf");
        QVERIFY(QFile::exists(finalPath));
        QVERIFY(!QFile::exists(finalPath + QStringLiteral(".part")));
        QFile f(finalPath);
        QVERIFY(f.open(QIODevice::ReadOnly));
        QCOMPARE(f.readAll(), body);
        QVERIFY(svc.recommendedPresent(QStringLiteral("embed")));
        QVERIFY(!svc.active());
        QVERIFY(!svc.start(QStringLiteral("embed")));
    }

    void test_hashMismatch_deletesAndFails() {
        const QByteArray body(4096, 'y');
        OneShotHttpServer server(body);
        QTemporaryDir dir;

        ModelDownloadService svc;
        svc.setTargetDirProvider([&dir](const QString&) { return dir.path(); });
        svc.setPinOverrideForTest(QStringLiteral("ragp"),
                                  server.url(),
                                  QStringLiteral("00").repeated(32),
                                  body.size(),
                                  QStringLiteral("fixture-ragp.gguf"));

        QSignalSpy done(&svc, &ModelDownloadService::downloadFinished);
        QSignalSpy failed(&svc, &ModelDownloadService::downloadFailed);
        QVERIFY(svc.start(QStringLiteral("ragp")));
        QVERIFY(failed.wait(10000));
        QCOMPARE(done.count(), 0);
        QVERIFY(failed.first().at(1).toString().contains(QStringLiteral("checksum")));
        const QStringList leftovers = QDir(dir.path()).entryList(QDir::Files);
        QVERIFY2(leftovers.isEmpty(), qPrintable(leftovers.join(QLatin1Char(','))));
        QVERIFY(!svc.lastError().isEmpty());
    }

    void test_sizeMismatch_deletesAndFails() {
        const QByteArray body(4096, 'z');
        OneShotHttpServer server(body);
        QTemporaryDir dir;

        ModelDownloadService svc;
        svc.setTargetDirProvider([&dir](const QString&) { return dir.path(); });
        svc.setPinOverrideForTest(QStringLiteral("ragp"),
                                  server.url(),
                                  sha256Hex(body),
                                  body.size() + 1,
                                  QStringLiteral("fixture-ragp.gguf"));

        QSignalSpy failed(&svc, &ModelDownloadService::downloadFailed);
        QVERIFY(svc.start(QStringLiteral("ragp")));
        QVERIFY(failed.wait(10000));
        QVERIFY(failed.first().at(1).toString().contains(QStringLiteral("size mismatch")));
        QVERIFY(QDir(dir.path()).entryList(QDir::Files).isEmpty());
    }

    void test_cancel_cleansPartial() {
        const QByteArray body(1024 * 1024, 'c');
        OneShotHttpServer server(body);
        QTemporaryDir dir;

        ModelDownloadService svc;
        svc.setTargetDirProvider([&dir](const QString&) { return dir.path(); });
        svc.setPinOverrideForTest(QStringLiteral("embed"),
                                  server.url(),
                                  sha256Hex(body),
                                  body.size(),
                                  QStringLiteral("fixture-embed.gguf"));

        QSignalSpy state(&svc, &ModelDownloadService::stateChanged);
        QSignalSpy done(&svc, &ModelDownloadService::downloadFinished);
        QSignalSpy failed(&svc, &ModelDownloadService::downloadFailed);
        QVERIFY(svc.start(QStringLiteral("embed")));
        svc.cancel();
        QTRY_VERIFY_WITH_TIMEOUT(!svc.active(), 10000);
        QCOMPARE(done.count(), 0);
        QCOMPARE(failed.count(), 0);
        QVERIFY(QDir(dir.path()).entryList(QDir::Files).isEmpty());
        Q_UNUSED(state);
    }
};

QTEST_MAIN(TestModelDownloadService)
#include "test-model-download-service.moc"
