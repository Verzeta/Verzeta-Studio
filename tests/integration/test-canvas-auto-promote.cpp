// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/db-manager.h"
#include "services/canvas-service.h"
#include "services/conversation-service.h"
#include "services/file-service.h"

#include <QTemporaryDir>
#include <QTest>

#include <QDateTime>
#include <QSqlDatabase>
#include <QStandardPaths>
#include <QUuid>

class TestCanvasAutoPromote : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<FileService> m_fileSvc;
    std::unique_ptr<CanvasService> m_canvas;

    QString m_activeConvId;

    static QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

    static QString blob(int approxBytes) {
        const QString unit = QString(50, QLatin1Char('A')) + QLatin1Char('\n');
        const int repeats = (approxBytes + unit.size() - 1) / unit.size();
        return unit.repeated(repeats);
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
        m_canvas->setActiveConversationIdGetter([this]() -> QString { return m_activeConvId; });
        m_activeConvId.clear();
    }

    void cleanup() {
        m_canvas.reset();
        m_fileSvc.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    }


    void test_eligible_promotes() {
        const QString convId = m_convSvc->createConversation(QStringLiteral("F: eligible"));
        const QString id = m_canvas->tryAutoPromote(convId, QStringLiteral("a.json"), blob(800));
        QVERIFY(!id.isEmpty());
        QVERIFY(!m_canvas->activeCanvasFor(convId).isEmpty());
    }

    void test_tooShort_skipped() {
        const QString convId = m_convSvc->createConversation(QStringLiteral("F: too short"));
        const QString id =
            m_canvas->tryAutoPromote(convId, QStringLiteral("a.json"), QStringLiteral("{}"));
        QVERIFY(id.isEmpty());
        QVERIFY(m_canvas->activeCanvasFor(convId).isEmpty());
    }

    void test_extensionNotAllowlisted_skipped() {
        const QString convId = m_convSvc->createConversation(QStringLiteral("F: bad ext"));
        const QString id =
            m_canvas->tryAutoPromote(convId, QStringLiteral("photo.png"), blob(1500));
        QVERIFY(id.isEmpty());
        QVERIFY(m_canvas->activeCanvasFor(convId).isEmpty());
    }

    void test_emptyConvId_skipped() {
        const QString id =
            m_canvas->tryAutoPromote(QString(), QStringLiteral("a.json"), blob(1500));
        QVERIFY(id.isEmpty());
    }

    void test_emptyFilename_skipped() {
        const QString convId = m_convSvc->createConversation(QStringLiteral("F: empty fname"));
        const QString id = m_canvas->tryAutoPromote(convId, QString(), blob(1500));
        QVERIFY(id.isEmpty());
    }


    void test_fileSaved_triggersAutoPromote() {
        const QString convId = m_convSvc->createConversation(QStringLiteral("F: subscription"));
        m_activeConvId = convId;

        m_fileSvc->setActiveConversation(convId);
        const QString dir = m_fileSvc->activeProjectDir();
        QVERIFY(!dir.isEmpty());

        const QString content = blob(900);
        const QString savedPath =
            m_fileSvc->saveGeneratedFile(QStringLiteral("auto.md"), content, dir);
        QVERIFY(!savedPath.isEmpty());

        const QVariantMap m = m_canvas->activeCanvasFor(convId);
        QCOMPARE(m.value(QStringLiteral("filename")).toString(), QStringLiteral("auto.md"));
        QCOMPARE(m.value(QStringLiteral("language")).toString(), QStringLiteral("markdown"));
    }

    void test_fileSaved_belowThreshold_doesNotPromote() {
        const QString convId =
            m_convSvc->createConversation(QStringLiteral("F: subscription short"));
        m_activeConvId = convId;
        m_fileSvc->setActiveConversation(convId);
        const QString dir = m_fileSvc->activeProjectDir();

        const QString savedPath =
            m_fileSvc->saveGeneratedFile(QStringLiteral("hi.md"), QStringLiteral("# tiny"), dir);
        QVERIFY(!savedPath.isEmpty());
        QVERIFY(m_canvas->activeCanvasFor(convId).isEmpty());
    }

    void test_openCanvas_doesNotRecurseViaAutoPromote() {
        const QString convId = m_convSvc->createConversation(QStringLiteral("F: re-entry"));
        m_activeConvId = convId;

        const QString id = m_canvas->openCanvas(
            convId, QStringLiteral("config.json"), QStringLiteral("json"), blob(800));
        QVERIFY(!id.isEmpty());

        const QVariantList h = m_canvas->historyForConversation(convId);
        QCOMPARE(h.size(), 1);
        QCOMPARE(h.first().toMap().value(QStringLiteral("id")).toString(), id);
    }


    void test_languageInference_majorExtensions() {
        const QString convId = m_convSvc->createConversation(QStringLiteral("F: lang inference"));
        struct Case {
            QString filename;
            QString expectedLanguage;
        };
        const QList<Case> cases = {
            {QStringLiteral("notes.md"), QStringLiteral("markdown")},
            {QStringLiteral("config.json"), QStringLiteral("json")},
            {QStringLiteral("script.py"), QStringLiteral("python")},
            {QStringLiteral("module.ts"), QStringLiteral("typescript")},
            {QStringLiteral("compose.yaml"), QStringLiteral("yaml")},
            {QStringLiteral("setup.sh"), QStringLiteral("shell")},
            {QStringLiteral("main.cpp"), QStringLiteral("cpp")},
            {QStringLiteral("UI.qml"), QStringLiteral("qml")},
            {QStringLiteral("CMakeLists.txt"), QStringLiteral("cmake")},
            {QStringLiteral("index.html"), QStringLiteral("html")},
            {QStringLiteral("style.css"), QStringLiteral("css")},
            {QStringLiteral("widget.h"), QStringLiteral("cpp")},
            {QStringLiteral("widget.hpp"), QStringLiteral("cpp")},
            {QStringLiteral("util.hh"), QStringLiteral("cpp")},
            {QStringLiteral("legacy.c"), QStringLiteral("c")},
            {QStringLiteral("engine.cc"), QStringLiteral("cpp")},
            {QStringLiteral("lib.rs"), QStringLiteral("rust")},
            {QStringLiteral("server.go"), QStringLiteral("go")},
            {QStringLiteral("Main.java"), QStringLiteral("java")},
            {QStringLiteral("App.kt"), QStringLiteral("kotlin")},
            {QStringLiteral("app.rb"), QStringLiteral("ruby")},
            {QStringLiteral("index.php"), QStringLiteral("php")},
            {QStringLiteral("Service.cs"), QStringLiteral("csharp")},
            {QStringLiteral("schema.sql"), QStringLiteral("sql")},
            {QStringLiteral("Dockerfile"), QStringLiteral("dockerfile")},
            {QStringLiteral("Makefile"), QStringLiteral("makefile")},
        };
        const QString content = blob(800);

        for (const Case& c : cases) {
            const QString id = m_canvas->tryAutoPromote(convId, c.filename, content);
            QVERIFY2(!id.isEmpty(),
                     qPrintable(QStringLiteral("expected promote for %1").arg(c.filename)));
            const QVariantMap m = m_canvas->activeCanvasFor(convId);
            QCOMPARE(m.value(QStringLiteral("language")).toString(), c.expectedLanguage);
        }
    }
};

QTEST_MAIN(TestCanvasAutoPromote)
#include "test-canvas-auto-promote.moc"
