// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/db-manager.h"
#include "services/canvas-service.h"
#include "services/conversation-service.h"

#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QString>
#include <QUuid>

class TestCanvasExport : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<CanvasService> m_canvas;

    static QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

  private slots:

    void initTestCase() { QVERIFY(m_tempDir.isValid()); }

    void init() {
        const QString dbPath =
            m_tempDir.path() +
            QStringLiteral("/test_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_convSvc = std::make_unique<ConversationService>(DbManager::instance());
        m_canvas = std::make_unique<CanvasService>(DbManager::instance());
        m_canvas->setConversationService(m_convSvc.get());
    }

    void cleanup() {
        m_canvas.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    }

    QString seedCanvas(const QString& filename, const QString& language, const QString& content) {
        const QString convId = m_convSvc->createConversation(QStringLiteral("Export Test"));
        Q_ASSERT(!convId.isEmpty());
        const QString canvasId = m_canvas->openCanvas(convId, filename, language, content);
        Q_ASSERT(!canvasId.isEmpty());
        return convId;
    }


    void test_roundTrip_writesActiveContentVerbatim() {
        const QString convId = seedCanvas(QStringLiteral("hello.py"),
                                          QStringLiteral("python"),
                                          QStringLiteral("print('hello world')\n"));

        const QString dest = m_tempDir.path() + QStringLiteral("/exported.py");
        QVERIFY(m_canvas->exportCanvasToFile(convId, dest));

        QFile read(dest);
        QVERIFY(read.exists());
        QVERIFY(read.open(QIODevice::ReadOnly));
        const QByteArray bytes = read.readAll();
        QCOMPARE(QString::fromUtf8(bytes), QStringLiteral("print('hello world')\n"));
    }


    void test_emptyConversationId_returnsFalseAndEmitsError() {
        QSignalSpy spy(m_canvas.get(), &CanvasService::errorOccurred);
        const QString dest = m_tempDir.path() + QStringLiteral("/never.txt");
        QCOMPARE(m_canvas->exportCanvasToFile(QString(), dest), false);
        QCOMPARE(spy.count(), 1);
        QVERIFY(!QFile::exists(dest));
    }


    void test_emptyDestination_returnsFalseAndEmitsError() {
        const QString convId = seedCanvas(
            QStringLiteral("a.txt"), QStringLiteral("plaintext"), QStringLiteral("hello"));

        QSignalSpy spy(m_canvas.get(), &CanvasService::errorOccurred);
        QCOMPARE(m_canvas->exportCanvasToFile(convId, QString()), false);
        QCOMPARE(spy.count(), 1);
    }


    void test_noActiveCanvas_returnsFalseAndEmitsError() {
        const QString convId = m_convSvc->createConversation(QStringLiteral("No Canvas"));
        QVERIFY(!convId.isEmpty());

        QSignalSpy spy(m_canvas.get(), &CanvasService::errorOccurred);
        const QString dest = m_tempDir.path() + QStringLiteral("/never.txt");
        QCOMPARE(m_canvas->exportCanvasToFile(convId, dest), false);
        QCOMPARE(spy.count(), 1);
        QVERIFY(!QFile::exists(dest));
    }


    void test_overwriteExistingFile_replacesContent() {
        const QString convId = seedCanvas(QStringLiteral("config.json"),
                                          QStringLiteral("json"),
                                          QStringLiteral("{\"updated\": true}"));

        const QString dest = m_tempDir.path() + QStringLiteral("/already-here.json");
        {
            QFile f(dest);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("{\"old\": true}");
            f.close();
        }
        QVERIFY(QFile::exists(dest));

        QVERIFY(m_canvas->exportCanvasToFile(convId, dest));

        QFile read(dest);
        QVERIFY(read.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(read.readAll()), QStringLiteral("{\"updated\": true}"));
    }


    void test_suggestedExportName_returnsActiveFilename() {
        const QString convId = seedCanvas(QStringLiteral("report.md"),
                                          QStringLiteral("markdown"),
                                          QStringLiteral("# Report\n\nHello."));
        QCOMPARE(m_canvas->suggestedExportName(convId), QStringLiteral("report.md"));
    }

    void test_suggestedExportName_emptyWhenNoCanvas() {
        const QString convId = m_convSvc->createConversation(QStringLiteral("Empty"));
        QCOMPARE(m_canvas->suggestedExportName(convId), QString());
        QCOMPARE(m_canvas->suggestedExportName(QString()), QString());
    }
};

QTEST_MAIN(TestCanvasExport)
#include "test-canvas-export.moc"
