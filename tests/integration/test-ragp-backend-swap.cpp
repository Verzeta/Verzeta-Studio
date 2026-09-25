// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/db-manager.h"
#include "services/chat-controller.h"
#include "services/conversation-service.h"
#include "services/export-service.h"
#include "services/message-service.h"
#include "services/model-router.h"
#include "services/settings-service.h"

#include <QTemporaryDir>
#include <QtTest>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QStandardPaths>

class TestRagpBackendSwap : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    QString m_dbPath;

    void openFreshDatabase() {
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(m_dbPath));
        QVERIFY(DbManager::instance().runMigrations());
    }

    void closeDatabase() {
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QFile::remove(m_dbPath);
    }

  private slots:

    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void cleanupTestCase() { QStandardPaths::setTestModeEnabled(false); }

    void init() {
        QVERIFY(m_tempDir.isValid());
        m_dbPath = m_tempDir.path() +
                   QStringLiteral("/ragpswap_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
        openFreshDatabase();
    }

    void cleanup() { closeDatabase(); }


    void testAutoFallbackToRemoteOnLoadFailure() {
        SettingsService settings(DbManager::instance());
        const QString modelsDir = settings.ragpModelsDir();
        QDir(modelsDir).removeRecursively();
        QVERIFY(QDir().mkpath(modelsDir));

        const QString fakeModel = QStringLiteral("not-a-real.gguf");
        const QString fakePath = QDir(modelsDir).filePath(fakeModel);
        {
            QFile f(fakePath);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("definitely not a GGUF header");
            f.close();
        }

        settings.setRagpLocalEnabled(true);
        settings.setRagpDefaultModelFilename(fakeModel);

        ModelRouter router(this);
        ConversationService convSvc(DbManager::instance(), this);
        MessageService msgSvc(DbManager::instance(), this);
        ExportService exportSvc(convSvc, msgSvc, this);
        ChatController ctrl(router, convSvc, msgSvc, exportSvc, this);

        QSignalSpy backendSpy(&ctrl, &ChatController::ragpBackendChanged);

        ctrl.configureRagpBackend(&settings);

        QCOMPARE(backendSpy.count(), 1);
        const QString firstName = backendSpy.takeFirst()[0].toString();
        QVERIFY2(firstName.startsWith(QStringLiteral("local:llama.cpp:")), qPrintable(firstName));

        QVERIFY2(backendSpy.wait(5000),
                 "ragpBackendChanged did NOT fire a second time within 5s — "
                 "the auto-fallback path is broken");

        const QString secondName = backendSpy.takeFirst()[0].toString();
        QVERIFY2(secondName.startsWith(QStringLiteral("remote:")), qPrintable(secondName));
    }


    void testRemoteChosenWhenLocalDisabled() {
        SettingsService settings(DbManager::instance());
        settings.setRagpLocalEnabled(false);

        ModelRouter router(this);
        ConversationService convSvc(DbManager::instance(), this);
        MessageService msgSvc(DbManager::instance(), this);
        ExportService exportSvc(convSvc, msgSvc, this);
        ChatController ctrl(router, convSvc, msgSvc, exportSvc, this);

        QSignalSpy backendSpy(&ctrl, &ChatController::ragpBackendChanged);
        ctrl.configureRagpBackend(&settings);

        QCOMPARE(backendSpy.count(), 1);
        const QString name = backendSpy.takeFirst()[0].toString();
        QVERIFY2(name.startsWith(QStringLiteral("remote:")), qPrintable(name));

        QTest::qWait(50);
        QCOMPARE(backendSpy.count(), 0);
    }


    void testDoubleConfigureIsSafe() {
        SettingsService settings(DbManager::instance());
        const QString modelsDir = settings.ragpModelsDir();
        QDir(modelsDir).removeRecursively();
        QVERIFY(QDir().mkpath(modelsDir));

        const QString name = QStringLiteral("rapid.gguf");
        {
            QFile f(QDir(modelsDir).filePath(name));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("x");
            f.close();
        }

        settings.setRagpLocalEnabled(true);
        settings.setRagpDefaultModelFilename(name);

        ModelRouter router(this);
        ConversationService convSvc(DbManager::instance(), this);
        MessageService msgSvc(DbManager::instance(), this);
        ExportService exportSvc(convSvc, msgSvc, this);
        ChatController ctrl(router, convSvc, msgSvc, exportSvc, this);

        QSignalSpy backendSpy(&ctrl, &ChatController::ragpBackendChanged);

        ctrl.configureRagpBackend(&settings);
        QCOMPARE(backendSpy.count(), 1);

        settings.setRagpLocalEnabled(false);
        ctrl.configureRagpBackend(&settings);

        QTest::qWait(200);

        QVERIFY2(backendSpy.count() >= 2 && backendSpy.count() <= 3,
                 qPrintable(QStringLiteral("Unexpected backend-swap signal count: %1 "
                                           "(expected 2 or 3 depending on loadFailed timing)")
                                .arg(backendSpy.count())));

        for (int i = 1; i < backendSpy.count(); ++i) {
            const QString name = backendSpy.at(i)[0].toString();
            QVERIFY2(name.startsWith(QStringLiteral("remote:")),
                     qPrintable(QStringLiteral("signal %1: %2").arg(i).arg(name)));
        }

        const QString liveName = ctrl.ragpBackendName();
        QVERIFY2(liveName.startsWith(QStringLiteral("remote:")), qPrintable(liveName));
    }
};

QTEST_MAIN(TestRagpBackendSwap)
#include "test-ragp-backend-swap.moc"
