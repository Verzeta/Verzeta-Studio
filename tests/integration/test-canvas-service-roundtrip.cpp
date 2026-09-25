// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/db-manager.h"
#include "services/canvas-service.h"
#include "services/conversation-service.h"
#include "services/file-service.h"

#include <QTemporaryDir>
#include <QTest>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStandardPaths>
#include <QUuid>

class TestCanvasServiceRoundtrip : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<FileService> m_fileSvc;
    std::unique_ptr<CanvasService> m_canvas;

    static QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

  private slots:

    void initTestCase() {
        QVERIFY(m_tempDir.isValid());
        QStandardPaths::setTestModeEnabled(true);
    }

    void cleanupTestCase() { QStandardPaths::setTestModeEnabled(false); }

    void init() {
        const QString dbPath =
            m_tempDir.path() +
            QStringLiteral("/test_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_convSvc = std::make_unique<ConversationService>(DbManager::instance());
        m_fileSvc = std::make_unique<FileService>();
        m_canvas = std::make_unique<CanvasService>(DbManager::instance());
        m_canvas->setConversationService(m_convSvc.get());
        m_canvas->setFileService(m_fileSvc.get());
    }

    void cleanup() {
        m_canvas.reset();
        m_fileSvc.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    }


    void test_openCanvas_newFilename_insertsRow_emitsOpened() {
        const QString convId =
            m_convSvc->createConversation(QStringLiteral("canvas-roundtrip:insert"));
        QVERIFY(!convId.isEmpty());

        QSignalSpy spy(m_canvas.get(), &CanvasService::canvasOpened);
        const QString id = m_canvas->openCanvas(convId,
                                                QStringLiteral("config.json"),
                                                QStringLiteral("json"),
                                                QStringLiteral("{\n  \"a\": 1\n}\n"));
        QVERIFY(!id.isEmpty());
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().at(0).toString(), convId);
        QCOMPARE(spy.first().at(1).toString(), id);

        const QVariantMap m = m_canvas->activeCanvasFor(convId);
        QCOMPARE(m.value(QStringLiteral("id")).toString(), id);
        QCOMPARE(m.value(QStringLiteral("filename")).toString(), QStringLiteral("config.json"));
        QCOMPARE(m.value(QStringLiteral("language")).toString(), QStringLiteral("json"));
        QCOMPARE(m.value(QStringLiteral("revision")).toInt(), 0);
        QCOMPARE(m.value(QStringLiteral("isArchived")).toBool(), false);
    }


    void test_openCanvas_sameFilename_upsertsRevision() {
        const QString convId =
            m_convSvc->createConversation(QStringLiteral("canvas-roundtrip:upsert"));
        const QString id1 = m_canvas->openCanvas(
            convId, QStringLiteral("doc.md"), QStringLiteral("markdown"), QStringLiteral("# v1"));
        QVERIFY(!id1.isEmpty());

        const QString id2 = m_canvas->openCanvas(
            convId, QStringLiteral("doc.md"), QStringLiteral("markdown"), QStringLiteral("# v2"));
        QCOMPARE(id2, id1);

        const QVariantMap m = m_canvas->activeCanvasFor(convId);
        QCOMPARE(m.value(QStringLiteral("revision")).toInt(), 1);
        QCOMPARE(m.value(QStringLiteral("content")).toString(), QStringLiteral("# v2"));

        QCOMPARE(m_canvas->historyForConversation(convId).size(), 1);
    }


    void test_openCanvas_differentFilename_archivesPrior() {
        const QString convId =
            m_convSvc->createConversation(QStringLiteral("canvas-roundtrip:archive"));
        const QString idA = m_canvas->openCanvas(
            convId, QStringLiteral("a.json"), QStringLiteral("json"), QStringLiteral("{}"));
        const QString idB = m_canvas->openCanvas(convId,
                                                 QStringLiteral("b.py"),
                                                 QStringLiteral("python"),
                                                 QStringLiteral("print('hi')\n"));
        QVERIFY(!idA.isEmpty() && !idB.isEmpty() && idA != idB);

        const QVariantMap active = m_canvas->activeCanvasFor(convId);
        QCOMPARE(active.value(QStringLiteral("id")).toString(), idB);
        QCOMPARE(active.value(QStringLiteral("filename")).toString(), QStringLiteral("b.py"));

        const QVariantList history = m_canvas->historyForConversation(convId);
        QCOMPARE(history.size(), 2);
        QCOMPARE(history.at(0).toMap().value(QStringLiteral("id")).toString(), idB);
        QCOMPARE(history.at(0).toMap().value(QStringLiteral("isArchived")).toBool(), false);
        QCOMPARE(history.at(1).toMap().value(QStringLiteral("id")).toString(), idA);
        QCOMPARE(history.at(1).toMap().value(QStringLiteral("isArchived")).toBool(), true);
    }


    void test_closeCanvas_archivesActive_emitsClosed() {
        const QString convId =
            m_convSvc->createConversation(QStringLiteral("canvas-roundtrip:close"));
        const QString id = m_canvas->openCanvas(
            convId, QStringLiteral("x.txt"), QStringLiteral("plaintext"), QStringLiteral("hello"));
        QVERIFY(!id.isEmpty());

        QSignalSpy spy(m_canvas.get(), &CanvasService::canvasClosed);
        QVERIFY(m_canvas->closeCanvas(convId));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().at(0).toString(), convId);
        QCOMPARE(spy.first().at(1).toString(), id);

        QVERIFY(m_canvas->activeCanvasFor(convId).isEmpty());

        const QVariantList history = m_canvas->historyForConversation(convId);
        QCOMPARE(history.size(), 1);
        QCOMPARE(history.first().toMap().value(QStringLiteral("isArchived")).toBool(), true);

        QVERIFY(!m_canvas->closeCanvas(convId));
    }

    void test_closeThenReopen_unarchives_andRevisionAdvances() {
        const QString convId =
            m_convSvc->createConversation(QStringLiteral("canvas-roundtrip:close+reopen"));
        const QString id = m_canvas->openCanvas(
            convId, QStringLiteral("x.txt"), QStringLiteral("plaintext"), QStringLiteral("v1"));
        QVERIFY(m_canvas->closeCanvas(convId));

        const QString id2 = m_canvas->openCanvas(
            convId, QStringLiteral("x.txt"), QStringLiteral("plaintext"), QStringLiteral("v2"));
        QCOMPARE(id2, id);

        const QVariantMap active = m_canvas->activeCanvasFor(convId);
        QCOMPARE(active.value(QStringLiteral("isArchived")).toBool(), false);
        QCOMPARE(active.value(QStringLiteral("revision")).toInt(), 1);
        QCOMPARE(active.value(QStringLiteral("content")).toString(), QStringLiteral("v2"));
    }


    void test_historyForConversation_isReverseChrono() {
        const QString convId =
            m_convSvc->createConversation(QStringLiteral("canvas-roundtrip:history"));
        m_canvas->openCanvas(
            convId, QStringLiteral("a"), QStringLiteral("json"), QStringLiteral("{}"));
        QTest::qWait(20);
        m_canvas->openCanvas(
            convId, QStringLiteral("b"), QStringLiteral("json"), QStringLiteral("{}"));
        QTest::qWait(20);
        m_canvas->openCanvas(
            convId, QStringLiteral("c"), QStringLiteral("json"), QStringLiteral("{}"));

        const QVariantList h = m_canvas->historyForConversation(convId);
        QCOMPARE(h.size(), 3);
        QCOMPARE(h.at(0).toMap().value(QStringLiteral("filename")).toString(), QStringLiteral("c"));
        QCOMPARE(h.at(1).toMap().value(QStringLiteral("filename")).toString(), QStringLiteral("b"));
        QCOMPARE(h.at(2).toMap().value(QStringLiteral("filename")).toString(), QStringLiteral("a"));
    }


    void test_openCanvas_writesDiskMirror() {
        const QString convId =
            m_convSvc->createConversation(QStringLiteral("canvas-roundtrip:disk mirror"));
        const QString content = QStringLiteral("{\n  \"hello\": \"world\"\n}\n");
        const QString id = m_canvas->openCanvas(
            convId, QStringLiteral("hello.json"), QStringLiteral("json"), content);
        QVERIFY(!id.isEmpty());

        m_fileSvc->setActiveConversation(convId);
        const QString dir = m_fileSvc->activeProjectDir();
        QVERIFY(!dir.isEmpty());

        const QString expectedPath = dir + QStringLiteral("/hello.json");
        QFile f(expectedPath);
        QVERIFY2(f.exists(),
                 qPrintable(QStringLiteral("disk mirror missing at %1").arg(expectedPath)));
        QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString readBack = QString::fromUtf8(f.readAll());
        f.close();
        QCOMPARE(readBack, content);
    }


    void test_openCanvas_emptyConvId_returnsEmpty() {
        QSignalSpy spy(m_canvas.get(), &CanvasService::canvasOpened);
        const QString id = m_canvas->openCanvas(
            QString(), QStringLiteral("a.json"), QStringLiteral("json"), QStringLiteral("{}"));
        QVERIFY(id.isEmpty());
        QCOMPARE(spy.count(), 0);
    }

    void test_openCanvas_emptyFilename_returnsEmpty() {
        const QString convId =
            m_convSvc->createConversation(QStringLiteral("canvas-roundtrip:empty fname"));
        QSignalSpy spy(m_canvas.get(), &CanvasService::canvasOpened);
        const QString id =
            m_canvas->openCanvas(convId, QString(), QStringLiteral("json"), QStringLiteral("{}"));
        QVERIFY(id.isEmpty());
        QCOMPARE(spy.count(), 0);
    }

    void test_openCanvas_emptyLanguage_returnsEmpty() {
        const QString convId =
            m_convSvc->createConversation(QStringLiteral("canvas-roundtrip:empty lang"));
        QSignalSpy spy(m_canvas.get(), &CanvasService::canvasOpened);
        const QString id =
            m_canvas->openCanvas(convId, QStringLiteral("a.json"), QString(), QStringLiteral("{}"));
        QVERIFY(id.isEmpty());
        QCOMPARE(spy.count(), 0);
    }


    void test_openCanvasFromFile_readsDisk_derivesLanguage() {
        const QString convId =
            m_convSvc->createConversation(QStringLiteral("canvas-roundtrip:from-file"));
        const QString filePath = m_tempDir.path() + QStringLiteral("/Marketing Notes.md");
        {
            QFile f(filePath);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("# Heading\n\nline two\n");
        }

        QSignalSpy spy(m_canvas.get(), &CanvasService::canvasOpened);
        const QString id = m_canvas->openCanvasFromFile(convId, filePath);
        QVERIFY(!id.isEmpty());
        QCOMPARE(spy.count(), 1);

        const QVariantMap m = m_canvas->activeCanvasFor(convId);
        QCOMPARE(m.value(QStringLiteral("filename")).toString(),
                 QStringLiteral("Marketing Notes.md"));
        QCOMPARE(m.value(QStringLiteral("language")).toString(), QStringLiteral("markdown"));
        QCOMPARE(m.value(QStringLiteral("content")).toString(),
                 QStringLiteral("# Heading\n\nline two\n"));
    }

    void test_openCanvasFromFile_unmappedExtension_plaintextFallback() {
        const QString convId =
            m_convSvc->createConversation(QStringLiteral("canvas-roundtrip:from-file-txt"));
        const QString filePath = m_tempDir.path() + QStringLiteral("/notes.txt");
        {
            QFile f(filePath);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("plain text body\n");
        }
        const QString id = m_canvas->openCanvasFromFile(convId, filePath);
        QVERIFY(!id.isEmpty());
        const QVariantMap m = m_canvas->activeCanvasFor(convId);
        QCOMPARE(m.value(QStringLiteral("language")).toString(), QStringLiteral("plaintext"));
    }

    void test_openCanvasFromFile_missingAndBinary_refused() {
        const QString convId =
            m_convSvc->createConversation(QStringLiteral("canvas-roundtrip:from-file-bad"));
        QSignalSpy spy(m_canvas.get(), &CanvasService::canvasOpened);

        QVERIFY(m_canvas->openCanvasFromFile(convId, m_tempDir.path() + QStringLiteral("/nope.md"))
                    .isEmpty());

        const QString binPath = m_tempDir.path() + QStringLiteral("/blob.md");
        {
            QFile f(binPath);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("abc\0def", 7);
        }
        QVERIFY(m_canvas->openCanvasFromFile(convId, binPath).isEmpty());
        QCOMPARE(spy.count(), 0);
    }

    void test_openCanvasFromWorkspace_opensExistingByName() {
        const QString convId =
            m_convSvc->createConversation(QStringLiteral("canvas-roundtrip:from-workspace"));
        m_fileSvc->setActiveConversation(convId);
        const QString dir = m_fileSvc->activeProjectDir();
        QVERIFY(QDir().mkpath(dir));
        const QString body = QStringLiteral("# Report\n\nbody line\n");
        {
            QFile f(dir + QStringLiteral("/Report.md"));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(body.toUtf8());
        }

        const QString id = m_canvas->openCanvasFromWorkspace(convId, QStringLiteral("Report.md"));
        QVERIFY(!id.isEmpty());
        const QVariantMap m = m_canvas->activeCanvasFor(convId);
        QCOMPARE(m.value(QStringLiteral("filename")).toString(), QStringLiteral("Report.md"));
        QCOMPARE(m.value(QStringLiteral("content")).toString(), body);
        QCOMPARE(m.value(QStringLiteral("language")).toString(), QStringLiteral("markdown"));
    }

    void test_openCanvasFromWorkspace_missing_returnsEmpty() {
        const QString convId = m_convSvc->createConversation(
            QStringLiteral("canvas-roundtrip:from-workspace-missing"));
        QVERIFY(
            m_canvas->openCanvasFromWorkspace(convId, QStringLiteral("DoesNotExist.md")).isEmpty());
    }

    void test_openCanvasFromWorkspace_traversalRejected() {
        const QString convId =
            m_convSvc->createConversation(QStringLiteral("canvas-roundtrip:from-workspace-escape"));
        QVERIFY(m_canvas->openCanvasFromWorkspace(convId, QStringLiteral("../../etc/hostname"))
                    .isEmpty());
    }

    void test_openCanvas_writeFailure_rollsBack_priorStaysActive() {
        const QString convId =
            m_convSvc->createConversation(QStringLiteral("canvas-roundtrip:atomic"));
        const QString idA = m_canvas->openCanvas(convId,
                                                 QStringLiteral("A.md"),
                                                 QStringLiteral("markdown"),
                                                 QStringLiteral("content A"));
        QVERIFY(!idA.isEmpty());

        const QString idB = m_canvas->openCanvas(
            convId, QStringLiteral("B.md"), QStringLiteral("markdown"), QString());
        QVERIFY(idB.isEmpty());

        const QVariantMap active = m_canvas->activeCanvasFor(convId);
        QVERIFY(!active.isEmpty());
        QCOMPARE(active.value(QStringLiteral("filename")).toString(), QStringLiteral("A.md"));
        QCOMPARE(active.value(QStringLiteral("id")).toString(), idA);
    }

    void test_retargetAndEdit_opensWorkspaceFileAndEdits() {
        const QString convId =
            m_convSvc->createConversation(QStringLiteral("canvas-roundtrip:retarget"));
        m_canvas->openCanvas(convId,
                             QStringLiteral("Other.md"),
                             QStringLiteral("markdown"),
                             QStringLiteral("other"));
        m_fileSvc->setActiveConversation(convId);
        const QString dir = m_fileSvc->activeProjectDir();
        QVERIFY(QDir().mkpath(dir));
        {
            QFile f(dir + QStringLiteral("/Sales.md"));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("old sales body\n");
        }

        const QString id = m_canvas->retargetAndEdit(
            convId, QStringLiteral("Sales.md"), QStringLiteral("## New Sales Body\n"));
        QVERIFY(!id.isEmpty());
        const QVariantMap active = m_canvas->activeCanvasFor(convId);
        QCOMPARE(active.value(QStringLiteral("filename")).toString(), QStringLiteral("Sales.md"));
        QCOMPARE(active.value(QStringLiteral("content")).toString(),
                 QStringLiteral("## New Sales Body\n"));
    }

    void test_retargetAndEdit_nonexistent_returnsEmpty() {
        const QString convId =
            m_convSvc->createConversation(QStringLiteral("canvas-roundtrip:retarget-missing"));
        QVERIFY(m_canvas->retargetAndEdit(convId, QStringLiteral("Ghost.md"), QStringLiteral("x"))
                    .isEmpty());
    }
};

QTEST_MAIN(TestCanvasServiceRoundtrip)
#include "test-canvas-service-roundtrip.moc"
