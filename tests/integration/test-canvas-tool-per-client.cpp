// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/db-manager.h"
#include "services/canvas-service.h"
#include "services/conversation-service.h"
#include "services/file-service.h"
#include "tools/canvas/edit-canvas-tool.h"
#include "tools/canvas/open-canvas-tool.h"
#include "tools/canvas/read-canvas-tool.h"

#include <QTemporaryDir>
#include <QTest>

#include <QDateTime>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QStandardPaths>

class TestCanvasToolPerClient : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<FileService> m_fileSvc;
    std::unique_ptr<CanvasService> m_canvas;

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


    void test_openCanvas_argsConvIdUsed_whenGetterEmpty() {
        const QString wireConvId = m_convSvc->createConversation(QStringLiteral("wire 1:1 chat"));
        QVERIFY(!wireConvId.isEmpty());

        Tools::OpenCanvasTool tool(*m_canvas, []() -> QString { return QString(); });
        QJsonObject args;
        args[QStringLiteral("filename")] = QStringLiteral("hello.py");
        args[QStringLiteral("language")] = QStringLiteral("python");
        args[QStringLiteral("content")] = QStringLiteral("print('hi')\n");
        args[QStringLiteral("__caller_conv_id")] = wireConvId;

        const QJsonObject r = tool.invoke(args).toObject();
        QVERIFY2(!r.contains(QStringLiteral("error")),
                 qPrintable(QStringLiteral("unexpected error: %1")
                                .arg(r.value(QStringLiteral("error")).toString())));
        QVERIFY(r.contains(QStringLiteral("canvas_id")));

        const QVariantMap active = m_canvas->activeCanvasFor(wireConvId);
        QVERIFY(!active.isEmpty());
        QCOMPARE(active.value(QStringLiteral("filename")).toString(), QStringLiteral("hello.py"));
    }

    void test_openCanvas_argsConvIdOverridesCapturedGetter() {
        const QString localConvId = m_convSvc->createConversation(QStringLiteral("local desktop"));
        const QString wireConvId = m_convSvc->createConversation(QStringLiteral("wire android"));
        QVERIFY(!localConvId.isEmpty());
        QVERIFY(!wireConvId.isEmpty());

        Tools::OpenCanvasTool tool(*m_canvas, [localConvId]() -> QString { return localConvId; });
        QJsonObject args;
        args[QStringLiteral("filename")] = QStringLiteral("wire.py");
        args[QStringLiteral("language")] = QStringLiteral("python");
        args[QStringLiteral("content")] = QStringLiteral("print('wire content')");
        args[QStringLiteral("__caller_conv_id")] = wireConvId;

        const QJsonObject r = tool.invoke(args).toObject();
        QVERIFY2(!r.contains(QStringLiteral("error")),
                 qPrintable(QStringLiteral("unexpected error: %1")
                                .arg(r.value(QStringLiteral("error")).toString())));

        const QVariantMap wireActive = m_canvas->activeCanvasFor(wireConvId);
        const QVariantMap localActive = m_canvas->activeCanvasFor(localConvId);
        QVERIFY(!wireActive.isEmpty());
        QVERIFY2(localActive.isEmpty(), "Canvas should NOT be active on LOCAL conv — args id wins");
        QCOMPARE(wireActive.value(QStringLiteral("filename")).toString(),
                 QStringLiteral("wire.py"));
    }

    void test_openCanvas_fallsBackToCapturedGetter_whenArgsEmpty() {
        const QString convId = m_convSvc->createConversation(QStringLiteral("legacy direct call"));
        Tools::OpenCanvasTool tool(*m_canvas, [convId]() -> QString { return convId; });
        QJsonObject args;
        args[QStringLiteral("filename")] = QStringLiteral("legacy.txt");
        args[QStringLiteral("language")] = QStringLiteral("plaintext");
        args[QStringLiteral("content")] = QStringLiteral("legacy body");

        const QJsonObject r = tool.invoke(args).toObject();
        QVERIFY2(!r.contains(QStringLiteral("error")),
                 qPrintable(QStringLiteral("unexpected error: %1")
                                .arg(r.value(QStringLiteral("error")).toString())));

        const QVariantMap active = m_canvas->activeCanvasFor(convId);
        QVERIFY(!active.isEmpty());
    }


    void test_editCanvas_argsConvIdOverridesCapturedGetter() {
        const QString localConvId = m_convSvc->createConversation(QStringLiteral("local"));
        const QString wireConvId = m_convSvc->createConversation(QStringLiteral("wire"));
        m_canvas->openCanvas(wireConvId,
                             QStringLiteral("hello.py"),
                             QStringLiteral("python"),
                             QStringLiteral("print('v1')"),
                             QString());

        Tools::EditCanvasTool tool(*m_canvas, [localConvId]() -> QString { return localConvId; });
        QJsonObject args;
        args[QStringLiteral("new_content")] = QStringLiteral("print('v2')");
        args[QStringLiteral("__caller_conv_id")] = wireConvId;

        const QJsonObject r = tool.invoke(args).toObject();
        QVERIFY2(!r.contains(QStringLiteral("error")),
                 qPrintable(
                     QStringLiteral("error: %1").arg(r.value(QStringLiteral("error")).toString())));

        const QVariantMap wireActive = m_canvas->activeCanvasFor(wireConvId);
        QCOMPARE(wireActive.value(QStringLiteral("content")).toString(),
                 QStringLiteral("print('v2')"));
    }


    void test_readCanvas_argsConvIdOverridesCapturedGetter() {
        const QString localConvId = m_convSvc->createConversation(QStringLiteral("local"));
        const QString wireConvId = m_convSvc->createConversation(QStringLiteral("wire"));
        m_canvas->openCanvas(wireConvId,
                             QStringLiteral("hello.py"),
                             QStringLiteral("python"),
                             QStringLiteral("print('wire content')"),
                             QString());

        Tools::ReadCanvasTool tool(*m_canvas, [localConvId]() -> QString { return localConvId; });
        QJsonObject args;
        args[QStringLiteral("__caller_conv_id")] = wireConvId;

        const QJsonObject r = tool.invoke(args).toObject();
        QVERIFY(!r.contains(QStringLiteral("error")));
        const QString content = r.value(QStringLiteral("content")).toString();
        QCOMPARE(content, QStringLiteral("print('wire content')"));
    }
};

QTEST_MAIN(TestCanvasToolPerClient)
#include "test-canvas-tool-per-client.moc"
