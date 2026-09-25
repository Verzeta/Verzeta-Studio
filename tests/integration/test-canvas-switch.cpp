// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/db-manager.h"
#include "services/canvas-service.h"
#include "services/conversation-service.h"
#include "services/file-service.h"

#include <QTemporaryDir>
#include <QTest>

#include <QDateTime>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QStandardPaths>
#include <QUuid>

class TestCanvasSwitch : public QObject {
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

    void test_switch_archivedCanvas_promotesIt() {
        const QString convId = m_convSvc->createConversation(QStringLiteral("G: switch"));
        const QString idA = m_canvas->openCanvas(
            convId, QStringLiteral("a.json"), QStringLiteral("json"), QStringLiteral("{}"));
        const QString idB = m_canvas->openCanvas(
            convId, QStringLiteral("b.py"), QStringLiteral("python"), QStringLiteral("print(1)"));
        QCOMPARE(m_canvas->activeCanvasFor(convId).value(QStringLiteral("id")).toString(), idB);

        QSignalSpy spy(m_canvas.get(), &CanvasService::canvasOpened);
        QVERIFY(m_canvas->switchToCanvas(convId, idA));
        QCOMPARE(m_canvas->activeCanvasFor(convId).value(QStringLiteral("id")).toString(), idA);

        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.first().at(1).toString(), idA);

        const QVariantList h = m_canvas->historyForConversation(convId);
        QCOMPARE(h.size(), 2);
        QCOMPARE(h.at(0).toMap().value(QStringLiteral("id")).toString(), idA);
        QCOMPARE(h.at(0).toMap().value(QStringLiteral("isArchived")).toBool(), false);
        QCOMPARE(h.at(1).toMap().value(QStringLiteral("id")).toString(), idB);
        QCOMPARE(h.at(1).toMap().value(QStringLiteral("isArchived")).toBool(), true);
    }

    void test_switch_alreadyActive_isNoop() {
        const QString convId = m_convSvc->createConversation(QStringLiteral("G: noop"));
        const QString idA = m_canvas->openCanvas(
            convId, QStringLiteral("a.json"), QStringLiteral("json"), QStringLiteral("{}"));

        QSignalSpy spy(m_canvas.get(), &CanvasService::canvasOpened);
        QVERIFY(m_canvas->switchToCanvas(convId, idA));
        QCOMPARE(m_canvas->activeCanvasFor(convId).value(QStringLiteral("id")).toString(), idA);
        QCOMPARE(spy.count(), 0);
    }

    void test_switch_unknownId_returnsFalse() {
        const QString convId = m_convSvc->createConversation(QStringLiteral("G: bogus"));
        QVERIFY(!m_canvas->switchToCanvas(convId, uuid()));
    }

    void test_switch_emptyArgs_returnsFalse() {
        const QString convId = m_convSvc->createConversation(QStringLiteral("G: empty"));
        const QString id = m_canvas->openCanvas(
            convId, QStringLiteral("a.json"), QStringLiteral("json"), QStringLiteral("{}"));
        QVERIFY(!m_canvas->switchToCanvas(QString(), id));
        QVERIFY(!m_canvas->switchToCanvas(convId, QString()));
    }

    void test_switch_threeCanvases_roundTrip() {
        const QString convId = m_convSvc->createConversation(QStringLiteral("G: round trip"));
        const QString idA = m_canvas->openCanvas(
            convId, QStringLiteral("a"), QStringLiteral("json"), QStringLiteral("{}"));
        const QString idB = m_canvas->openCanvas(
            convId, QStringLiteral("b"), QStringLiteral("json"), QStringLiteral("{}"));
        const QString idC = m_canvas->openCanvas(
            convId, QStringLiteral("c"), QStringLiteral("json"), QStringLiteral("{}"));
        QCOMPARE(m_canvas->activeCanvasFor(convId).value(QStringLiteral("id")).toString(), idC);

        QVERIFY(m_canvas->switchToCanvas(convId, idA));
        QCOMPARE(m_canvas->activeCanvasFor(convId).value(QStringLiteral("id")).toString(), idA);

        QVERIFY(m_canvas->switchToCanvas(convId, idB));
        QCOMPARE(m_canvas->activeCanvasFor(convId).value(QStringLiteral("id")).toString(), idB);

        QVERIFY(m_canvas->switchToCanvas(convId, idC));
        QCOMPARE(m_canvas->activeCanvasFor(convId).value(QStringLiteral("id")).toString(), idC);
    }
};

QTEST_MAIN(TestCanvasSwitch)
#include "test-canvas-switch.moc"
