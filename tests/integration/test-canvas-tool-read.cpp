// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/db-manager.h"
#include "services/canvas-service.h"
#include "services/conversation-service.h"
#include "services/file-service.h"
#include "tools/canvas/read-canvas-tool.h"

#include <QTemporaryDir>
#include <QTest>

#include <QDateTime>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QStandardPaths>
#include <QUuid>

class TestCanvasToolRead : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<FileService> m_fileSvc;
    std::unique_ptr<CanvasService> m_canvas;

    QString m_activeConvId;

    static QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

    Tools::ReadCanvasTool makeTool() {
        return Tools::ReadCanvasTool(*m_canvas, [this]() -> QString { return m_activeConvId; });
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

    void test_noActiveConversation_returnsError() {
        Tools::ReadCanvasTool tool = makeTool();
        m_activeConvId.clear();
        const QJsonObject r = tool.invoke({}).toObject();
        QVERIFY(r.contains(QStringLiteral("error")));
    }

    void test_noCanvas_returnsError() {
        m_activeConvId = m_convSvc->createConversation(QStringLiteral("read: no canvas"));
        Tools::ReadCanvasTool tool = makeTool();
        const QJsonObject r = tool.invoke({}).toObject();
        QCOMPARE(r.value(QStringLiteral("error")).toString(),
                 QStringLiteral("No active canvas in this conversation"));
    }

    void test_defaultArgs_returnsFullContent() {
        m_activeConvId = m_convSvc->createConversation(QStringLiteral("read: full"));
        const QString content = QStringLiteral("a\nb\nc\nd\ne");
        QVERIFY(
            !m_canvas
                 ->openCanvas(
                     m_activeConvId, QStringLiteral("f.txt"), QStringLiteral("plaintext"), content)
                 .isEmpty());

        Tools::ReadCanvasTool tool = makeTool();
        const QJsonObject r = tool.invoke({}).toObject();
        QVERIFY(!r.contains(QStringLiteral("error")));
        QCOMPARE(r.value(QStringLiteral("content")).toString(), content);
        QCOMPARE(r.value(QStringLiteral("start_line")).toInt(), 1);
        QCOMPARE(r.value(QStringLiteral("end_line")).toInt(), 5);
        QCOMPARE(r.value(QStringLiteral("total_lines")).toInt(), 5);
    }

    void test_specificRange_returnsSlice() {
        m_activeConvId = m_convSvc->createConversation(QStringLiteral("read: slice"));
        const QString content = QStringLiteral("L1\nL2\nL3\nL4\nL5\nL6");
        QVERIFY(
            !m_canvas
                 ->openCanvas(
                     m_activeConvId, QStringLiteral("f.txt"), QStringLiteral("plaintext"), content)
                 .isEmpty());

        Tools::ReadCanvasTool tool = makeTool();
        QJsonObject args;
        args[QStringLiteral("start_line")] = 2;
        args[QStringLiteral("end_line")] = 4;
        const QJsonObject r = tool.invoke(args).toObject();
        QCOMPARE(r.value(QStringLiteral("content")).toString(), QStringLiteral("L2\nL3\nL4"));
        QCOMPARE(r.value(QStringLiteral("start_line")).toInt(), 2);
        QCOMPARE(r.value(QStringLiteral("end_line")).toInt(), 4);
        QCOMPARE(r.value(QStringLiteral("total_lines")).toInt(), 6);
    }

    void test_endLineMinusOne_readsToEof() {
        m_activeConvId = m_convSvc->createConversation(QStringLiteral("read: -1 sentinel"));
        const QString content = QStringLiteral("x\ny\nz");
        QVERIFY(
            !m_canvas
                 ->openCanvas(
                     m_activeConvId, QStringLiteral("f.txt"), QStringLiteral("plaintext"), content)
                 .isEmpty());

        Tools::ReadCanvasTool tool = makeTool();
        QJsonObject args;
        args[QStringLiteral("start_line")] = 2;
        args[QStringLiteral("end_line")] = -1;
        const QJsonObject r = tool.invoke(args).toObject();
        QCOMPARE(r.value(QStringLiteral("content")).toString(), QStringLiteral("y\nz"));
        QCOMPARE(r.value(QStringLiteral("end_line")).toInt(), 3);
    }

    void test_outOfRange_returnsInvalidRangeError() {
        m_activeConvId = m_convSvc->createConversation(QStringLiteral("read: out of range"));
        const QString content = QStringLiteral("a\nb");
        QVERIFY(
            !m_canvas
                 ->openCanvas(
                     m_activeConvId, QStringLiteral("f.txt"), QStringLiteral("plaintext"), content)
                 .isEmpty());

        Tools::ReadCanvasTool tool = makeTool();
        QJsonObject args;
        args[QStringLiteral("start_line")] = 99;
        const QJsonObject r = tool.invoke(args).toObject();
        QCOMPARE(r.value(QStringLiteral("error")).toString(), QStringLiteral("Invalid line range"));
        QCOMPARE(r.value(QStringLiteral("total_lines")).toInt(), 2);
    }

    void test_endBeforeStart_returnsInvalidRangeError() {
        m_activeConvId = m_convSvc->createConversation(QStringLiteral("read: reverse range"));
        const QString content = QStringLiteral("a\nb\nc");
        QVERIFY(
            !m_canvas
                 ->openCanvas(
                     m_activeConvId, QStringLiteral("f.txt"), QStringLiteral("plaintext"), content)
                 .isEmpty());

        Tools::ReadCanvasTool tool = makeTool();
        QJsonObject args;
        args[QStringLiteral("start_line")] = 3;
        args[QStringLiteral("end_line")] = 1;
        const QJsonObject r = tool.invoke(args).toObject();
        QCOMPARE(r.value(QStringLiteral("error")).toString(), QStringLiteral("Invalid line range"));
    }
};

QTEST_MAIN(TestCanvasToolRead)
#include "test-canvas-tool-read.moc"
