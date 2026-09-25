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

class TestCanvasToolsActions : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<FileService> m_fileSvc;
    std::unique_ptr<CanvasService> m_canvas;

    static QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

    QString makeConvWithCanvas(const QString& title,
                               const QString& filename,
                               const QString& language,
                               const QString& content) {
        const QString convId = m_convSvc->createConversation(title);
        Q_ASSERT(!convId.isEmpty());
        const QString id = m_canvas->openCanvas(convId, filename, language, content);
        Q_ASSERT(!id.isEmpty());
        return convId;
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
    }

    void cleanup() {
        m_canvas.reset();
        m_fileSvc.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    }


    void test_actionsForJson_validateAndFormatBothEnabled() {
        const QVariantList actions = m_canvas->availableActionsForLanguage(QStringLiteral("json"));
        QCOMPARE(actions.size(), 2);
        QCOMPARE(actions.at(0).toMap().value(QStringLiteral("id")).toString(),
                 QStringLiteral("validate"));
        QCOMPARE(actions.at(0).toMap().value(QStringLiteral("enabled")).toBool(), true);
        QCOMPARE(actions.at(1).toMap().value(QStringLiteral("id")).toString(),
                 QStringLiteral("format"));
        QCOMPARE(actions.at(1).toMap().value(QStringLiteral("enabled")).toBool(), true);
    }

    void test_actionsForPython_validateEnabled_formatDisabled() {
        const QVariantList actions =
            m_canvas->availableActionsForLanguage(QStringLiteral("python"));
        QCOMPARE(actions.size(), 2);
        QCOMPARE(actions.at(0).toMap().value(QStringLiteral("enabled")).toBool(), true);
        QCOMPARE(actions.at(1).toMap().value(QStringLiteral("enabled")).toBool(), false);
    }

    void test_actionsForPlaintext_validateEnabled_formatDisabled() {
        const QVariantList actions =
            m_canvas->availableActionsForLanguage(QStringLiteral("plaintext"));
        QCOMPARE(actions.size(), 2);
        QCOMPARE(actions.at(0).toMap().value(QStringLiteral("enabled")).toBool(), true);
        QCOMPARE(actions.at(1).toMap().value(QStringLiteral("enabled")).toBool(), false);
    }


    void test_validate_validJson_returnsOk() {
        const QString convId = makeConvWithCanvas(QStringLiteral("E: validate ok"),
                                                  QStringLiteral("a.json"),
                                                  QStringLiteral("json"),
                                                  QStringLiteral("{\n  \"k\": 1\n}"));
        const QVariantMap r = m_canvas->performAction(convId, QStringLiteral("validate"));
        QCOMPARE(r.value(QStringLiteral("ok")).toBool(), true);
        QCOMPARE(r.value(QStringLiteral("mutated")).toBool(), false);
    }

    void test_validate_invalidJson_returnsErrorLine() {
        const QString convId = makeConvWithCanvas(QStringLiteral("E: validate fail"),
                                                  QStringLiteral("a.json"),
                                                  QStringLiteral("json"),
                                                  QStringLiteral("{\n  \"k\": ,\n}"));
        const QVariantMap r = m_canvas->performAction(convId, QStringLiteral("validate"));
        QCOMPARE(r.value(QStringLiteral("ok")).toBool(), false);
        QVERIFY(r.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("JSON parse error")));
        QVERIFY(r.value(QStringLiteral("errorLine")).toInt() >= 1);
    }

    void test_validate_plaintext_passesAsNoop() {
        const QString convId = makeConvWithCanvas(QStringLiteral("E: validate plaintext"),
                                                  QStringLiteral("notes.txt"),
                                                  QStringLiteral("plaintext"),
                                                  QStringLiteral("anything goes"));
        const QVariantMap r = m_canvas->performAction(convId, QStringLiteral("validate"));
        QCOMPARE(r.value(QStringLiteral("ok")).toBool(), true);
    }


    void test_format_uglyJson_reformatsAndBumpsRevision() {
        const QString convId = makeConvWithCanvas(QStringLiteral("E: format ugly"),
                                                  QStringLiteral("a.json"),
                                                  QStringLiteral("json"),
                                                  QStringLiteral("{\"k\":1,\"v\":[1,2,3]}"));
        const int rev0 =
            m_canvas->activeCanvasFor(convId).value(QStringLiteral("revision")).toInt();

        const QVariantMap r = m_canvas->performAction(convId, QStringLiteral("format"));
        QCOMPARE(r.value(QStringLiteral("ok")).toBool(), true);
        QCOMPARE(r.value(QStringLiteral("mutated")).toBool(), true);

        const QVariantMap m = m_canvas->activeCanvasFor(convId);
        QCOMPARE(m.value(QStringLiteral("revision")).toInt(), rev0 + 1);
        const QString c = m.value(QStringLiteral("content")).toString();
        QVERIFY(c.contains(QLatin1Char('\n')));
        QVERIFY(c.contains(QStringLiteral("    \"k\": 1")) ||
                c.contains(QStringLiteral("    \"k\":1")));
    }

    void test_format_alreadyFormatted_isNoop() {
        const QString pretty = QStringLiteral("{\n    \"k\": 1\n}");
        const QString convId = makeConvWithCanvas(QStringLiteral("E: format noop"),
                                                  QStringLiteral("a.json"),
                                                  QStringLiteral("json"),
                                                  pretty);
        const int rev0 =
            m_canvas->activeCanvasFor(convId).value(QStringLiteral("revision")).toInt();

        const QVariantMap r = m_canvas->performAction(convId, QStringLiteral("format"));
        QCOMPARE(r.value(QStringLiteral("ok")).toBool(), true);
        QCOMPARE(r.value(QStringLiteral("mutated")).toBool(), false);
        QCOMPARE(m_canvas->activeCanvasFor(convId).value(QStringLiteral("revision")).toInt(), rev0);
    }

    void test_format_invalidJson_returnsErrorAndDoesNotMutate() {
        const QString broken = QStringLiteral("{not json}");
        const QString convId = makeConvWithCanvas(QStringLiteral("E: format invalid"),
                                                  QStringLiteral("a.json"),
                                                  QStringLiteral("json"),
                                                  broken);
        const int rev0 =
            m_canvas->activeCanvasFor(convId).value(QStringLiteral("revision")).toInt();

        const QVariantMap r = m_canvas->performAction(convId, QStringLiteral("format"));
        QCOMPARE(r.value(QStringLiteral("ok")).toBool(), false);
        QVERIFY(r.contains(QStringLiteral("errorLine")));
        QCOMPARE(m_canvas->activeCanvasFor(convId).value(QStringLiteral("revision")).toInt(), rev0);
        QCOMPARE(m_canvas->activeCanvasFor(convId).value(QStringLiteral("content")).toString(),
                 broken);
    }

    void test_format_pythonLanguage_returnsNotAvailable() {
        const QString convId = makeConvWithCanvas(QStringLiteral("E: format python"),
                                                  QStringLiteral("a.py"),
                                                  QStringLiteral("python"),
                                                  QStringLiteral("def f(): return 1"));
        const QVariantMap r = m_canvas->performAction(convId, QStringLiteral("format"));
        QCOMPARE(r.value(QStringLiteral("ok")).toBool(), false);
        QVERIFY(r.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("Format not available")));
    }


    void test_unknownAction_returnsError() {
        const QString convId = makeConvWithCanvas(QStringLiteral("E: unknown"),
                                                  QStringLiteral("a.json"),
                                                  QStringLiteral("json"),
                                                  QStringLiteral("{}"));
        const QVariantMap r = m_canvas->performAction(convId, QStringLiteral("nonsense"));
        QCOMPARE(r.value(QStringLiteral("ok")).toBool(), false);
        QVERIFY(r.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("Unknown action")));
    }

    void test_emptyConvId_returnsError() {
        const QVariantMap r = m_canvas->performAction(QString(), QStringLiteral("validate"));
        QCOMPARE(r.value(QStringLiteral("ok")).toBool(), false);
    }

    void test_noActiveCanvas_returnsError() {
        const QString convId = m_convSvc->createConversation(QStringLiteral("E: no canvas"));
        const QVariantMap r = m_canvas->performAction(convId, QStringLiteral("validate"));
        QCOMPARE(r.value(QStringLiteral("ok")).toBool(), false);
        QVERIFY(r.value(QStringLiteral("message"))
                    .toString()
                    .contains(QStringLiteral("No active canvas")));
    }
};

QTEST_MAIN(TestCanvasToolsActions)
#include "test-canvas-tools-actions.moc"
