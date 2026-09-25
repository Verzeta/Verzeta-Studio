// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/db-manager.h"
#include "services/canvas-service.h"
#include "services/conversation-service.h"
#include "services/file-service.h"
#include "tools/canvas/edit-canvas-tool.h"

#include <QTemporaryDir>
#include <QTest>

#include <QDateTime>
#include <QJsonObject>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QStandardPaths>
#include <QUuid>

class TestCanvasToolEdit : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<FileService> m_fileSvc;
    std::unique_ptr<CanvasService> m_canvas;
    QString m_activeConvId;

    Tools::EditCanvasTool makeTool() {
        return Tools::EditCanvasTool(*m_canvas, [this]() -> QString { return m_activeConvId; });
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
        m_activeConvId.clear();
    }

    void cleanup() {
        m_canvas.reset();
        m_fileSvc.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    }

    void test_noActiveConv_returnsError() {
        Tools::EditCanvasTool tool = makeTool();
        QJsonObject args;
        args[QStringLiteral("new_content")] = QStringLiteral("foo");
        const QJsonObject r = tool.invoke(args).toObject();
        QVERIFY(r.contains(QStringLiteral("error")));
    }

    void test_noActiveCanvas_returnsError() {
        m_activeConvId = m_convSvc->createConversation(QStringLiteral("edit: no canvas"));
        Tools::EditCanvasTool tool = makeTool();
        QJsonObject args;
        args[QStringLiteral("new_content")] = QStringLiteral("foo");
        const QJsonObject r = tool.invoke(args).toObject();
        QCOMPARE(r.value(QStringLiteral("error")).toString(),
                 QStringLiteral("No active canvas in this conversation"));
    }

    void test_missingNewContent_returnsError() {
        m_activeConvId = m_convSvc->createConversation(QStringLiteral("edit: missing"));
        m_canvas->openCanvas(
            m_activeConvId, QStringLiteral("a"), QStringLiteral("plaintext"), QStringLiteral("hi"));
        Tools::EditCanvasTool tool = makeTool();
        const QJsonObject r = tool.invoke({}).toObject();
        QCOMPARE(r.value(QStringLiteral("error")).toString(),
                 QStringLiteral("new_content is required"));
    }

    void test_successfulEdit_bumpsRevision_emitsSignal() {
        m_activeConvId = m_convSvc->createConversation(QStringLiteral("edit: ok"));
        const QString id = m_canvas->openCanvas(
            m_activeConvId, QStringLiteral("a.json"), QStringLiteral("json"), QStringLiteral("{}"));
        QVERIFY(!id.isEmpty());
        QCOMPARE(
            m_canvas->activeCanvasFor(m_activeConvId).value(QStringLiteral("revision")).toInt(), 0);

        QSignalSpy spy(m_canvas.get(), &CanvasService::canvasUpdated);
        Tools::EditCanvasTool tool = makeTool();
        QJsonObject args;
        args[QStringLiteral("new_content")] = QStringLiteral("{\n  \"k\": 1\n}");
        const QJsonObject r = tool.invoke(args).toObject();

        QVERIFY(!r.contains(QStringLiteral("error")));
        QCOMPARE(r.value(QStringLiteral("canvas_id")).toString(), id);
        QCOMPARE(r.value(QStringLiteral("revision")).toInt(), 1);
        QCOMPARE(r.value(QStringLiteral("lines")).toInt(), 3);
        QVERIFY(r.value(QStringLiteral("byte_size")).toInt() > 0);

        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().at(2).toInt(), 1);

        const QVariantMap m = m_canvas->activeCanvasFor(m_activeConvId);
        QCOMPARE(m.value(QStringLiteral("revision")).toInt(), 1);
        QCOMPARE(m.value(QStringLiteral("content")).toString(), QStringLiteral("{\n  \"k\": 1\n}"));
    }

    void test_mismatchedExistingCanvas_retargets() {
        m_activeConvId = m_convSvc->createConversation(QStringLiteral("edit: retarget-existing"));
        m_canvas->openCanvas(m_activeConvId,
                             QStringLiteral("b.md"),
                             QStringLiteral("markdown"),
                             QStringLiteral("b0"));
        m_canvas->openCanvas(m_activeConvId,
                             QStringLiteral("a.md"),
                             QStringLiteral("markdown"),
                             QStringLiteral("a0"));
        Tools::EditCanvasTool tool = makeTool();
        QJsonObject args;
        args[QStringLiteral("new_content")] = QStringLiteral("b1");
        args[QStringLiteral("filename")] = QStringLiteral("b.md");
        const QJsonObject r = tool.invoke(args).toObject();

        QVERIFY(!r.contains(QStringLiteral("error")));
        const QVariantMap m = m_canvas->activeCanvasFor(m_activeConvId);
        QCOMPARE(m.value(QStringLiteral("filename")).toString(), QStringLiteral("b.md"));
        QCOMPARE(m.value(QStringLiteral("content")).toString(), QStringLiteral("b1"));
    }

    void test_mismatchedNonexistent_refused_activeUnchanged() {
        m_activeConvId = m_convSvc->createConversation(QStringLiteral("edit: retarget-missing"));
        m_canvas->openCanvas(m_activeConvId,
                             QStringLiteral("a.md"),
                             QStringLiteral("markdown"),
                             QStringLiteral("original"));
        Tools::EditCanvasTool tool = makeTool();
        QJsonObject args;
        args[QStringLiteral("new_content")] = QStringLiteral("nope");
        args[QStringLiteral("filename")] = QStringLiteral("ghost.md");
        const QJsonObject r = tool.invoke(args).toObject();

        QVERIFY(r.contains(QStringLiteral("error")));
        const QVariantMap m = m_canvas->activeCanvasFor(m_activeConvId);
        QCOMPARE(m.value(QStringLiteral("filename")).toString(), QStringLiteral("a.md"));
        QCOMPARE(m.value(QStringLiteral("content")).toString(), QStringLiteral("original"));
    }

    void test_matchingFilename_proceeds() {
        m_activeConvId = m_convSvc->createConversation(QStringLiteral("edit: target-match"));
        m_canvas->openCanvas(m_activeConvId,
                             QStringLiteral("a.md"),
                             QStringLiteral("markdown"),
                             QStringLiteral("v0"));
        Tools::EditCanvasTool tool = makeTool();
        QJsonObject args;
        args[QStringLiteral("new_content")] = QStringLiteral("v1");
        args[QStringLiteral("filename")] = QStringLiteral("a.md");
        const QJsonObject r = tool.invoke(args).toObject();

        QVERIFY(!r.contains(QStringLiteral("error")));
        QCOMPARE(
            m_canvas->activeCanvasFor(m_activeConvId).value(QStringLiteral("content")).toString(),
            QStringLiteral("v1"));
    }

    void test_consecutiveEdits_revisionAdvances() {
        m_activeConvId = m_convSvc->createConversation(QStringLiteral("edit: x2"));
        m_canvas->openCanvas(
            m_activeConvId, QStringLiteral("a"), QStringLiteral("plaintext"), QStringLiteral("v0"));

        Tools::EditCanvasTool tool = makeTool();
        QJsonObject a;
        a[QStringLiteral("new_content")] = QStringLiteral("v1");
        QCOMPARE(tool.invoke(a).toObject().value(QStringLiteral("revision")).toInt(), 1);

        QJsonObject b;
        b[QStringLiteral("new_content")] = QStringLiteral("v2");
        QCOMPARE(tool.invoke(b).toObject().value(QStringLiteral("revision")).toInt(), 2);
    }
};

QTEST_MAIN(TestCanvasToolEdit)
#include "test-canvas-tool-edit.moc"
