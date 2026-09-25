// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/db-manager.h"
#include "services/canvas-ai-actions.h"
#include "services/canvas-service.h"
#include "services/conversation-service.h"

#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <QDateTime>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>

class TestCanvasAiActions : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<CanvasService> m_canvas;
    std::unique_ptr<CanvasAiActions> m_actions;
    QString m_convId;

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

        m_actions = std::make_unique<CanvasAiActions>();
        m_actions->setCanvasService(m_canvas.get());

        m_convId = m_convSvc->createConversation(QStringLiteral("AI Test"));
        QVERIFY(!m_convId.isEmpty());
        m_actions->setActiveConversationIdGetter([this]() { return m_convId; });
    }

    void cleanup() {
        m_actions.reset();
        m_canvas.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    }

    QStringList idsFor(const QString& language) {
        const QVariantList list = m_actions->availableForLanguage(language);
        QStringList ids;
        for (const QVariant& v : list)
            ids.append(v.toMap().value(QStringLiteral("id")).toString());
        return ids;
    }
    int countPrimary(const QString& language) {
        const QVariantList list = m_actions->availableForLanguage(language);
        int n = 0;
        for (const QVariant& v : list) {
            if (v.toMap().value(QStringLiteral("primary")).toBool())
                ++n;
        }
        return n;
    }


    void test_codeFamily_includesAllPrimaryActions() {
        const QStringList ids = idsFor(QStringLiteral("python"));
        QVERIFY(ids.contains(QStringLiteral("code.add-comments")));
        QVERIFY(ids.contains(QStringLiteral("code.fix-bugs")));
        QVERIFY(ids.contains(QStringLiteral("code.refactor")));
        QVERIFY(ids.contains(QStringLiteral("code.port")));
        QCOMPARE(countPrimary(QStringLiteral("python")), 4);
    }

    void test_proseFamily_includesAllPrimaryActions() {
        const QStringList ids = idsFor(QStringLiteral("markdown"));
        QVERIFY(ids.contains(QStringLiteral("prose.autocomplete")));
        QVERIFY(ids.contains(QStringLiteral("prose.grammify")));
        QVERIFY(ids.contains(QStringLiteral("prose.summarize")));
        QVERIFY(ids.contains(QStringLiteral("prose.spin")));
        QCOMPARE(countPrimary(QStringLiteral("markdown")), 4);
    }

    void test_dataFamily_includesAllPrimaryActions() {
        const QStringList ids = idsFor(QStringLiteral("yaml"));
        QVERIFY(ids.contains(QStringLiteral("data.convert")));
        QVERIFY(ids.contains(QStringLiteral("data.generate-schema")));
        QVERIFY(ids.contains(QStringLiteral("data.annotate")));
        QVERIFY(ids.contains(QStringLiteral("data.from-description")));
        QCOMPARE(countPrimary(QStringLiteral("yaml")), 4);
    }

    void test_jsonHidesAnnotateAndAddComments() {
        const QStringList ids = idsFor(QStringLiteral("json"));
        QVERIFY(!ids.contains(QStringLiteral("data.annotate")));
        QVERIFY(!ids.contains(QStringLiteral("data.add-comments")));
        QVERIFY(ids.contains(QStringLiteral("data.convert")));
        QVERIFY(ids.contains(QStringLiteral("data.generate-schema")));
        QVERIFY(ids.contains(QStringLiteral("data.from-description")));
    }

    void test_unknownLanguage_returnsEmpty() {
        QCOMPARE(m_actions->availableForLanguage(QString()).size(), 0);
        QCOMPARE(m_actions->availableForLanguage(QStringLiteral("klingon")).size(), 0);
    }

    void test_portToLanguage_carriesSubmenu() {
        const QVariantList list = m_actions->availableForLanguage(QStringLiteral("python"));
        QVariantMap port;
        for (const QVariant& v : list) {
            const auto m = v.toMap();
            if (m.value(QStringLiteral("id")).toString() == QStringLiteral("code.port")) {
                port = m;
                break;
            }
        }
        const QStringList submenu = port.value(QStringLiteral("submenu")).toStringList();
        QVERIFY(!submenu.isEmpty());
        QVERIFY(submenu.contains(QStringLiteral("python")));
        QVERIFY(submenu.contains(QStringLiteral("typescript")));
        QVERIFY(submenu.contains(QStringLiteral("rust")));
    }


    void seedCanvas(const QString& filename, const QString& language, const QString& content) {
        const QString id = m_canvas->openCanvas(m_convId, filename, language, content);
        QVERIFY(!id.isEmpty());
    }

    void test_trigger_emptyId_emitsErrorAndReturnsFalse() {
        seedCanvas(QStringLiteral("a.py"), QStringLiteral("python"), QStringLiteral("print(1)"));
        QSignalSpy spy(m_actions.get(), &CanvasAiActions::errorOccurred);
        QCOMPARE(m_actions->trigger(QString(), QString()), false);
        QCOMPARE(spy.count(), 1);
    }

    void test_trigger_unknownId_emitsErrorAndReturnsFalse() {
        seedCanvas(QStringLiteral("a.py"), QStringLiteral("python"), QStringLiteral("print(1)"));
        QSignalSpy spy(m_actions.get(), &CanvasAiActions::errorOccurred);
        QCOMPARE(m_actions->trigger(QStringLiteral("nope.nada"), QString()), false);
        QCOMPARE(spy.count(), 1);
    }

    void test_trigger_noActiveCanvas_emitsErrorAndReturnsFalse() {
        QSignalSpy spy(m_actions.get(), &CanvasAiActions::errorOccurred);
        QCOMPARE(m_actions->trigger(QStringLiteral("code.add-comments"), QString()), false);
        QCOMPARE(spy.count(), 1);
    }

    void test_trigger_validAction_succeeds() {
        seedCanvas(
            QStringLiteral("hello.py"), QStringLiteral("python"), QStringLiteral("print(1)"));
        QCOMPARE(m_actions->trigger(QStringLiteral("code.add-comments"), QString()), true);
    }
};

QTEST_MAIN(TestCanvasAiActions)
#include "test-canvas-ai-actions.moc"
