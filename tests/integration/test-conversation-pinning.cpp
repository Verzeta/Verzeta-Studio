// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/conversation-list-model.h"
#include "models/conversation.h"
#include "models/db-manager.h"
#include "services/conversation-service.h"

#include <QTemporaryDir>
#include <QTest>

#include <QDateTime>
#include <QFile>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QString>
#include <QStringList>

class TestConversationPinning : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    QString m_dbPath;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<ConversationListModel> m_listModel;

    void openFreshDb() {
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        m_dbPath = m_tempDir.path() +
                   QStringLiteral("/test_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
        QVERIFY(DbManager::instance().open(m_dbPath));
        QVERIFY(DbManager::instance().runMigrations());
        m_convSvc = std::make_unique<ConversationService>(DbManager::instance());
        m_listModel = std::make_unique<ConversationListModel>(*m_convSvc);
    }

  private slots:

    void initTestCase() { QVERIFY(m_tempDir.isValid()); }

    void init() { openFreshDb(); }

    void cleanup() {
        m_listModel.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        if (!m_dbPath.isEmpty())
            QFile::remove(m_dbPath);
    }


    void test_migrationAddsIsPinnedColumn() {
        QSqlDatabase db = DbManager::instance().db();

        QSqlQuery q(db);
        QVERIFY(q.exec(QStringLiteral("PRAGMA table_info(conversations)")));

        bool found = false;
        bool notNull = false;
        QString defaultValue;
        QString columnType;
        while (q.next()) {
            if (q.value(QStringLiteral("name")).toString() == QStringLiteral("is_pinned")) {
                found = true;
                columnType = q.value(QStringLiteral("type")).toString();
                notNull = q.value(QStringLiteral("notnull")).toInt() != 0;
                defaultValue = q.value(QStringLiteral("dflt_value")).toString();
                break;
            }
        }
        QVERIFY2(found, "is_pinned column missing on conversations table");
        QCOMPARE(columnType, QStringLiteral("INTEGER"));
        QVERIFY2(notNull, "is_pinned must be NOT NULL");
        QCOMPARE(defaultValue, QStringLiteral("0"));
    }

    void test_migrationCreatesPinnedIndex() {
        QSqlDatabase db = DbManager::instance().db();
        QSqlQuery q(db);
        QVERIFY(q.exec(QStringLiteral("SELECT name FROM sqlite_master "
                                      "WHERE type='index' AND name='idx_conversations_pinned'")));
        QVERIFY2(q.next(), "idx_conversations_pinned index missing");
    }

    void test_schemaVersionReachesNine() {
        QSqlQuery q(DbManager::instance().db());
        QVERIFY(q.exec(
            QStringLiteral("SELECT value FROM settings WHERE key='schema_version' LIMIT 1")));
        QVERIFY(q.next());
        QVERIFY2(
            q.value(0).toInt() >= 9,
            qPrintable(QStringLiteral("expected schema >= 9, got %1").arg(q.value(0).toInt())));
    }


    void test_newConversationIsUnpinnedByDefault() {
        const QString convId = m_convSvc->createConversation(QStringLiteral("Default Pin Test"));
        QVERIFY(!convId.isEmpty());

        const auto opt = m_convSvc->getConversation(convId);
        QVERIFY(opt.has_value());
        QCOMPARE(opt->isPinned, false);
    }


    void test_setConversationPinnedTogglesAndEmits() {
        const QString convId = m_convSvc->createConversation(QStringLiteral("Pin Test"));
        QVERIFY(!convId.isEmpty());

        QSignalSpy spy(m_convSvc.get(), &ConversationService::conversationUpdated);

        QVERIFY(m_convSvc->setConversationPinned(convId, true));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toString(), convId);
        {
            const auto opt = m_convSvc->getConversation(convId);
            QVERIFY(opt.has_value());
            QCOMPARE(opt->isPinned, true);
        }

        QVERIFY(m_convSvc->setConversationPinned(convId, false));
        QCOMPARE(spy.count(), 2);
        {
            const auto opt = m_convSvc->getConversation(convId);
            QVERIFY(opt.has_value());
            QCOMPARE(opt->isPinned, false);
        }
    }

    void test_setConversationPinnedBumpsUpdatedAt() {
        const QString convId = m_convSvc->createConversation(QStringLiteral("Bump Test"));
        QVERIFY(!convId.isEmpty());

        const auto preOpt = m_convSvc->getConversation(convId);
        QVERIFY(preOpt.has_value());
        const QDateTime beforeUpdated = preOpt->updatedAt;

        QTest::qWait(2);

        QVERIFY(m_convSvc->setConversationPinned(convId, true));

        const auto postOpt = m_convSvc->getConversation(convId);
        QVERIFY(postOpt.has_value());
        QVERIFY2(postOpt->updatedAt > beforeUpdated,
                 "updated_at must move forward when pin is toggled");
    }


    void test_multipleTogglesPreserveLatestValue() {
        const QString convId = m_convSvc->createConversation(QStringLiteral("Toggle Test"));
        QVERIFY(!convId.isEmpty());

        QVERIFY(m_convSvc->setConversationPinned(convId, true));
        QVERIFY(m_convSvc->setConversationPinned(convId, false));
        QVERIFY(m_convSvc->setConversationPinned(convId, true));
        QVERIFY(m_convSvc->setConversationPinned(convId, true));

        const auto opt = m_convSvc->getConversation(convId);
        QVERIFY(opt.has_value());
        QCOMPARE(opt->isPinned, true);
    }


    void test_pinFlagPersistsAcrossDbReopen() {
        const QString convId = m_convSvc->createConversation(QStringLiteral("Persistence Test"));
        QVERIFY(!convId.isEmpty());
        QVERIFY(m_convSvc->setConversationPinned(convId, true));

        m_listModel.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));

        QVERIFY(DbManager::instance().open(m_dbPath));
        QVERIFY(DbManager::instance().runMigrations());
        m_convSvc = std::make_unique<ConversationService>(DbManager::instance());
        m_listModel = std::make_unique<ConversationListModel>(*m_convSvc);

        const auto opt = m_convSvc->getConversation(convId);
        QVERIFY(opt.has_value());
        QCOMPARE(opt->isPinned, true);
    }


    QModelIndex findConversationIndex(const QString& convId) const {
        const int rootCount = m_listModel->rowCount();
        for (int i = 0; i < rootCount; ++i) {
            const QModelIndex idx = m_listModel->index(i, 0);
            if (m_listModel->data(idx, ConversationListModel::TypeRole).toString() ==
                    QStringLiteral("conversation") &&
                m_listModel->data(idx, ConversationListModel::IdRole).toString() == convId) {
                return idx;
            }
            const int childCount = m_listModel->rowCount(idx);
            for (int j = 0; j < childCount; ++j) {
                const QModelIndex c = m_listModel->index(j, 0, idx);
                if (m_listModel->data(c, ConversationListModel::IdRole).toString() == convId) {
                    return c;
                }
            }
        }
        return {};
    }

    void test_listModelExposesIsPinnedRole() {
        const QString convId = m_convSvc->createConversation(QStringLiteral("Role Test"));
        QVERIFY(!convId.isEmpty());

        const QModelIndex idx = findConversationIndex(convId);
        QVERIFY(idx.isValid());

        QCOMPARE(m_listModel->data(idx, ConversationListModel::IsPinnedRole).toBool(), false);

        QSignalSpy dcSpy(m_listModel.get(), &ConversationListModel::dataChanged);

        QVERIFY(m_convSvc->setConversationPinned(convId, true));

        const QModelIndex idxAfter = findConversationIndex(convId);
        QVERIFY(idxAfter.isValid());
        QCOMPARE(m_listModel->data(idxAfter, ConversationListModel::IsPinnedRole).toBool(), true);

        bool sawRole = false;
        for (int i = 0; i < dcSpy.count(); ++i) {
            const QVariantList args = dcSpy.at(i);
            const QList<int> roles = args.at(2).value<QList<int>>();
            if (roles.contains(ConversationListModel::IsPinnedRole)) {
                sawRole = true;
                break;
            }
        }
        QVERIFY2(sawRole, "dataChanged signal must include IsPinnedRole on toggle");

        const QHash<int, QByteArray> names = m_listModel->roleNames();
        QCOMPARE(names.value(ConversationListModel::IsPinnedRole), QByteArrayLiteral("isPinned"));
    }


    void test_setOnMissingIdReturnsFalseNoSignal() {
        QSignalSpy spy(m_convSvc.get(), &ConversationService::conversationUpdated);
        QCOMPARE(m_convSvc->setConversationPinned(QStringLiteral("does-not-exist"), true), false);
        QCOMPARE(spy.count(), 0);
    }


    void test_setOnEmptyIdReturnsFalseNoSignal() {
        QSignalSpy spy(m_convSvc.get(), &ConversationService::conversationUpdated);
        QCOMPARE(m_convSvc->setConversationPinned(QString(), true), false);
        QCOMPARE(spy.count(), 0);
    }
};

QTEST_MAIN(TestConversationPinning)
#include "test-conversation-pinning.moc"
