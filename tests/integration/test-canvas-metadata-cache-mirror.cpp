// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/db-manager.h"
#include "services/canvas-service.h"
#include "services/conversation-service.h"
#include "services/file-service.h"

#include <QTemporaryDir>
#include <QTest>

#include <QDateTime>
#include <QObject>
#include <QSqlDatabase>
#include <QStandardPaths>
#include <QUuid>

namespace {

struct CacheSink {
    QString filename;
    QString language;
    int revision = 0;
    int lineCount = 0;
    qint64 byteSize = 0;

    bool isEmpty() const { return filename.isEmpty(); }
    void clear() {
        filename.clear();
        language.clear();
        revision = 0;
        lineCount = 0;
        byteSize = 0;
    }
};

}  // namespace

class TestCanvasMetadataCacheMirror : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<FileService> m_fileSvc;
    std::unique_ptr<CanvasService> m_canvas;

    QString m_activeConvId;
    CacheSink m_sink;

    std::function<void(const QString&)> makeRefreshIfActive() {
        return [this](const QString& convId) {
            if (convId.isEmpty()) {
                m_sink.clear();
                return;
            }
            if (convId != m_activeConvId)
                return;
            const QVariantMap m = m_canvas->activeCanvasFor(convId);
            if (m.isEmpty()) {
                m_sink.clear();
                return;
            }
            m_sink.filename = m.value(QStringLiteral("filename")).toString();
            m_sink.language = m.value(QStringLiteral("language")).toString();
            m_sink.revision = m.value(QStringLiteral("revision")).toInt();
            m_sink.lineCount = m.value(QStringLiteral("lineCount")).toInt();
            m_sink.byteSize = m.value(QStringLiteral("byteSize")).toLongLong();
        };
    }

    void wireAdapter() {
        auto refresh = makeRefreshIfActive();
        QObject::connect(m_canvas.get(),
                         &CanvasService::canvasOpened,
                         this,
                         [refresh](const QString& c, const QString&) { refresh(c); });
        QObject::connect(m_canvas.get(),
                         &CanvasService::canvasUpdated,
                         this,
                         [refresh](const QString& c, const QString&, int) { refresh(c); });
        QObject::connect(m_canvas.get(),
                         &CanvasService::canvasClosed,
                         this,
                         [refresh](const QString& c, const QString&) { refresh(c); });
    }

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

        m_sink.clear();
        m_activeConvId.clear();
        wireAdapter();
    }

    void cleanup() {
        m_canvas.reset();
        m_fileSvc.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    }

    void test_openCanvas_inActive_populatesSink() {
        m_activeConvId = m_convSvc->createConversation(QStringLiteral("D: open in A"));
        QVERIFY(!m_canvas
                     ->openCanvas(m_activeConvId,
                                  QStringLiteral("a.json"),
                                  QStringLiteral("json"),
                                  QStringLiteral("{}"))
                     .isEmpty());
        QCOMPARE(m_sink.filename, QStringLiteral("a.json"));
        QCOMPARE(m_sink.language, QStringLiteral("json"));
        QCOMPARE(m_sink.revision, 0);
    }

    void test_editCanvas_inActive_bumpsRevision() {
        m_activeConvId = m_convSvc->createConversation(QStringLiteral("D: edit"));
        m_canvas->openCanvas(
            m_activeConvId, QStringLiteral("a.json"), QStringLiteral("json"), QStringLiteral("{}"));
        QVERIFY(m_canvas->editCanvas(m_activeConvId, QStringLiteral("{\"x\":1}")));
        QCOMPARE(m_sink.revision, 1);
        QVERIFY(m_sink.byteSize >= 7);
    }

    void test_closeCanvas_inActive_clearsSink() {
        m_activeConvId = m_convSvc->createConversation(QStringLiteral("D: close"));
        m_canvas->openCanvas(
            m_activeConvId, QStringLiteral("a.json"), QStringLiteral("json"), QStringLiteral("{}"));
        QVERIFY(!m_sink.isEmpty());
        QVERIFY(m_canvas->closeCanvas(m_activeConvId));
        QVERIFY(m_sink.isEmpty());
    }

    void test_openCanvas_inNonActive_doesNotTouchSink() {
        const QString convA = m_convSvc->createConversation(QStringLiteral("D: A"));
        const QString convB = m_convSvc->createConversation(QStringLiteral("D: B"));
        m_activeConvId = convA;

        m_canvas->openCanvas(
            convA, QStringLiteral("a.json"), QStringLiteral("json"), QStringLiteral("{}"));
        const CacheSink before = m_sink;

        m_canvas->openCanvas(
            convB, QStringLiteral("b.py"), QStringLiteral("python"), QStringLiteral("print('hi')"));
        QCOMPARE(m_sink.filename, before.filename);
        QCOMPARE(m_sink.language, before.language);
        QCOMPARE(m_sink.revision, before.revision);
    }

    void test_switchActive_pullsNewConvCanvas() {
        const QString convA = m_convSvc->createConversation(QStringLiteral("D: A"));
        const QString convB = m_convSvc->createConversation(QStringLiteral("D: B"));

        m_activeConvId = convA;
        m_canvas->openCanvas(
            convA, QStringLiteral("a.json"), QStringLiteral("json"), QStringLiteral("{}"));
        m_activeConvId = convB;
        m_canvas->openCanvas(
            convB, QStringLiteral("b.py"), QStringLiteral("python"), QStringLiteral("print(1)\n"));

        QCOMPARE(m_sink.filename, QStringLiteral("b.py"));
        QCOMPARE(m_sink.language, QStringLiteral("python"));

        m_activeConvId = convA;
        makeRefreshIfActive()(convA);
        QCOMPARE(m_sink.filename, QStringLiteral("a.json"));
    }

    void test_switchActive_toCanvaslessConv_clearsSink() {
        const QString convA = m_convSvc->createConversation(QStringLiteral("D: A"));
        const QString convC = m_convSvc->createConversation(QStringLiteral("D: C (no canvas)"));

        m_activeConvId = convA;
        m_canvas->openCanvas(
            convA, QStringLiteral("a.json"), QStringLiteral("json"), QStringLiteral("{}"));
        QVERIFY(!m_sink.isEmpty());

        m_activeConvId = convC;
        makeRefreshIfActive()(convC);
        QVERIFY(m_sink.isEmpty());
    }
};

QTEST_MAIN(TestCanvasMetadataCacheMirror)
#include "test-canvas-metadata-cache-mirror.moc"
