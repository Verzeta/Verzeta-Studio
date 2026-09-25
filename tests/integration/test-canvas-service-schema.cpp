// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/db-manager.h"
#include "services/canvas-service.h"
#include "services/conversation-service.h"

#include <QTemporaryDir>
#include <QTest>

#include <QDateTime>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QStringList>
#include <QUuid>
#include <QVariantMap>

class TestCanvasServiceSchema : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<CanvasService> m_canvas;

    static QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

    static bool seedCanvasRow(const QString& id,
                              const QString& convId,
                              const QString& filename,
                              const QString& content,
                              bool archived = false) {
        QSqlQuery q(DbManager::instance().db());
        q.prepare(QStringLiteral("INSERT INTO canvas_artifacts ("
                                 "  id, conversation_id, filename, language, content, "
                                 "  revision, is_archived, source_msg_id, created_at, updated_at"
                                 ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
        q.addBindValue(id);
        q.addBindValue(convId);
        q.addBindValue(filename);
        q.addBindValue(QStringLiteral("json"));
        q.addBindValue(content);
        q.addBindValue(0);
        q.addBindValue(archived ? 1 : 0);
        q.addBindValue(QString());
        const QString now = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        q.addBindValue(now);
        q.addBindValue(now);
        return q.exec();
    }

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


    void test_migrationCreatesCanvasArtifactsTable() {
        QSqlDatabase db = DbManager::instance().db();
        const QStringList tables = db.tables();
        QVERIFY2(tables.contains(QStringLiteral("canvas_artifacts")),
                 "canvas_artifacts table missing after runMigrations");
    }

    void test_canvasArtifactsHasExpectedColumns() {
        QSqlDatabase db = DbManager::instance().db();
        const QSqlRecord record = db.record(QStringLiteral("canvas_artifacts"));
        const QStringList expected = {
            QStringLiteral("id"),
            QStringLiteral("conversation_id"),
            QStringLiteral("filename"),
            QStringLiteral("language"),
            QStringLiteral("content"),
            QStringLiteral("revision"),
            QStringLiteral("is_archived"),
            QStringLiteral("source_msg_id"),
            QStringLiteral("created_at"),
            QStringLiteral("updated_at"),
        };
        for (const QString& col : expected) {
            QVERIFY2(record.contains(col),
                     qPrintable(QStringLiteral("canvas_artifacts.%1 column missing").arg(col)));
        }
    }

    void test_schemaVersionIsAtLeast8() {
        QSqlQuery q(DbManager::instance().db());
        QVERIFY(q.exec(
            QStringLiteral("SELECT value FROM settings WHERE key='schema_version' LIMIT 1")));
        QVERIFY(q.next());
        QVERIFY2(
            q.value(0).toInt() >= 8,
            qPrintable(
                QStringLiteral("schema_version is %1, expected >= 8").arg(q.value(0).toInt())));
    }

    void test_idxCanvasActiveExists() {
        QSqlQuery q(DbManager::instance().db());
        q.prepare(QStringLiteral("SELECT name FROM sqlite_master "
                                 "WHERE type='index' AND name='idx_canvas_active' "
                                 "  AND tbl_name='canvas_artifacts'"));
        QVERIFY(q.exec());
        QVERIFY2(q.next(), "idx_canvas_active index missing on canvas_artifacts");
    }


    void test_activeCanvasFor_emptyConvId_returnsEmpty() {
        const QVariantMap m = m_canvas->activeCanvasFor(QString());
        QVERIFY(m.isEmpty());
    }

    void test_activeCanvasFor_unknownConvId_returnsEmpty() {
        const QVariantMap m = m_canvas->activeCanvasFor(uuid());
        QVERIFY(m.isEmpty());
    }

    void test_historyForConversation_unknownConvId_returnsEmpty() {
        const QVariantList list = m_canvas->historyForConversation(uuid());
        QVERIFY(list.isEmpty());
    }

    void test_historyForConversation_emptyConvId_returnsEmpty() {
        const QVariantList list = m_canvas->historyForConversation(QString());
        QVERIFY(list.isEmpty());
    }


    void test_editCanvas_phaseC_pendingReturnsFalse() {
        QVERIFY(!m_canvas->editCanvas(uuid(), QStringLiteral("foo")));
    }

    void test_switchToCanvas_phaseG_pendingReturnsFalse() {
        QVERIFY(!m_canvas->switchToCanvas(uuid(), uuid()));
    }

    void test_readCanvasSlice_phaseC_pendingReturnsEmpty() {
        const QString slice = m_canvas->readCanvasSlice(uuid(), 1, -1);
        QVERIFY(slice.isEmpty());
    }


    void test_canvasRows_cascadeDelete_onConversationDeletion() {
        const QString convId = m_convSvc->createConversation(QStringLiteral("FK cascade test"));
        QVERIFY(!convId.isEmpty());

        const QString canvas1 = uuid();
        const QString canvas2 = uuid();
        QVERIFY(seedCanvasRow(
            canvas1, convId, QStringLiteral("a.json"), QStringLiteral("{\"n\":1}"), false));
        QVERIFY(seedCanvasRow(
            canvas2, convId, QStringLiteral("b.json"), QStringLiteral("{\"n\":2}"), true));

        QCOMPARE(m_canvas->historyForConversation(convId).size(), 2);
        const QVariantMap active = m_canvas->activeCanvasFor(convId);
        QCOMPARE(active.value(QStringLiteral("filename")).toString(), QStringLiteral("a.json"));

        QVERIFY(m_convSvc->deleteConversation(convId));

        QSqlQuery q(DbManager::instance().db());
        q.prepare(
            QStringLiteral("SELECT COUNT(*) FROM canvas_artifacts WHERE conversation_id = ?"));
        q.addBindValue(convId);
        QVERIFY(q.exec());
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 0);

        QVERIFY(m_canvas->activeCanvasFor(convId).isEmpty());
        QVERIFY(m_canvas->historyForConversation(convId).isEmpty());
    }
};

QTEST_MAIN(TestCanvasServiceSchema)
#include "test-canvas-service-schema.moc"
