// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/conversation-list-model.h"
#include "models/db-manager.h"
#include "services/conversation-service.h"

#include <QTemporaryDir>
#include <QTest>

#include <QDateTime>
#include <QSignalSpy>
#include <QSqlDatabase>

class TestConversationListModel : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    std::unique_ptr<ConversationService> m_svc;
    std::unique_ptr<ConversationListModel> m_model;

  private slots:

    void init() {
        QVERIFY(m_tempDir.isValid());
        const QString dbPath =
            m_tempDir.path() +
            QStringLiteral("/test_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_svc = std::make_unique<ConversationService>(DbManager::instance());
        m_model = std::make_unique<ConversationListModel>(*m_svc);
    }

    void cleanup() {
        m_model.reset();
        m_svc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    }

    void test_initialState_isEmpty() {
        QCOMPARE(m_model->rowCount(), 0);
        QCOMPARE(m_model->totalCount(), 0);
    }

    void test_conversationCreated_insertsRowNoReset() {
        QSignalSpy insertedSpy(m_model.get(), &QAbstractItemModel::rowsInserted);
        QSignalSpy resetSpy(m_model.get(), &QAbstractItemModel::modelReset);
        QSignalSpy countSpy(m_model.get(), &ConversationListModel::totalCountChanged);

        const QString id = m_svc->createConversation(QStringLiteral("Test Conv"));
        QVERIFY(!id.isEmpty());

        QCOMPARE(insertedSpy.count(), 1);
        QCOMPARE(resetSpy.count(), 0);
        QCOMPARE(m_model->rowCount(), 1);
        QCOMPARE(m_model->totalCount(), 1);
        QCOMPARE(countSpy.count(), 1);
    }

    void test_conversationUpdated_emitsDataChangedNoReset() {
        const QString id = m_svc->createConversation(QStringLiteral("Original"));

        QSignalSpy dataSpy(m_model.get(), &QAbstractItemModel::dataChanged);
        QSignalSpy resetSpy(m_model.get(), &QAbstractItemModel::modelReset);

        QVERIFY(m_svc->renameConversation(id, QStringLiteral("Renamed")));

        QVERIFY(dataSpy.count() >= 1);
        QCOMPARE(resetSpy.count(), 0);

        const QModelIndex idx = m_model->index(0, 0);
        QCOMPARE(m_model->data(idx, ConversationListModel::TitleRole).toString(),
                 QStringLiteral("Renamed"));
    }

    void test_conversationDeleted_removesRow() {
        const QString id = m_svc->createConversation(QStringLiteral("doomed"));
        QCOMPARE(m_model->rowCount(), 1);

        QSignalSpy removedSpy(m_model.get(), &QAbstractItemModel::rowsRemoved);
        QSignalSpy resetSpy(m_model.get(), &QAbstractItemModel::modelReset);

        QVERIFY(m_svc->deleteConversation(id));

        QCOMPARE(removedSpy.count(), 1);
        QCOMPARE(resetSpy.count(), 0);
        QCOMPARE(m_model->rowCount(), 0);
        QCOMPARE(m_model->totalCount(), 0);
    }

    void test_folderCreated_insertsFolderRow() {
        QSignalSpy insertedSpy(m_model.get(), &QAbstractItemModel::rowsInserted);
        const QString fid = m_svc->createFolder(QStringLiteral("Projects"));
        QVERIFY(!fid.isEmpty());

        QCOMPARE(insertedSpy.count(), 1);
        QCOMPARE(m_model->rowCount(), 1);

        const QModelIndex idx = m_model->index(0, 0);
        QCOMPARE(m_model->data(idx, ConversationListModel::TypeRole).toString(),
                 QStringLiteral("folder"));
        QCOMPARE(m_model->data(idx, ConversationListModel::TitleRole).toString(),
                 QStringLiteral("Projects"));
    }

    void test_folderDeleted_reparentsChildrenToRoot() {
        const QString fid = m_svc->createFolder(QStringLiteral("Group"));
        const QString cid = m_svc->createConversation(QStringLiteral("Child"), fid);

        QCOMPARE(m_model->rowCount(), 1);
        const QModelIndex folderIdx = m_model->index(0, 0);
        QCOMPARE(m_model->rowCount(folderIdx), 1);

        QVERIFY(m_svc->deleteFolder(fid));
        QCOMPARE(m_model->rowCount(), 1);
        const QModelIndex rootConvIdx = m_model->index(0, 0);
        QCOMPARE(m_model->data(rootConvIdx, ConversationListModel::TypeRole).toString(),
                 QStringLiteral("conversation"));
        QCOMPARE(m_model->data(rootConvIdx, ConversationListModel::IdRole).toString(), cid);
    }

    void test_conversationMovedBetweenFolders_usesBeginMoveRows() {
        const QString f1 = m_svc->createFolder(QStringLiteral("A"));
        const QString f2 = m_svc->createFolder(QStringLiteral("B"));
        const QString cid = m_svc->createConversation(QStringLiteral("Child"), f1);

        QSignalSpy movedSpy(m_model.get(), &QAbstractItemModel::rowsMoved);

        QVERIFY(m_svc->moveToFolder(cid, f2));

        QVERIFY(movedSpy.count() >= 1);

        QModelIndex folderA;
        QModelIndex folderB;
        for (int i = 0; i < m_model->rowCount(); ++i) {
            const QModelIndex idx = m_model->index(i, 0);
            const QString name = m_model->data(idx, ConversationListModel::TitleRole).toString();
            if (name == QStringLiteral("A"))
                folderA = idx;
            if (name == QStringLiteral("B"))
                folderB = idx;
        }
        QVERIFY(folderA.isValid());
        QVERIFY(folderB.isValid());
        QCOMPARE(m_model->rowCount(folderA), 0);
        QCOMPARE(m_model->rowCount(folderB), 1);
    }

    void test_touchConversation_movesRowToTop() {
        const QString a = m_svc->createConversation(QStringLiteral("A"));
        QTest::qWait(5);
        const QString b = m_svc->createConversation(QStringLiteral("B"));
        QTest::qWait(5);
        const QString c = m_svc->createConversation(QStringLiteral("C"));

        QCOMPARE(m_model->rowCount(), 3);
        QCOMPARE(m_model->data(m_model->index(0, 0), ConversationListModel::IdRole).toString(), c);

        QSignalSpy movedSpy(m_model.get(), &QAbstractItemModel::rowsMoved);
        QTest::qWait(10);

        QVERIFY(m_svc->touchConversation(a));

        QVERIFY(movedSpy.count() >= 1);
        QCOMPARE(m_model->data(m_model->index(0, 0), ConversationListModel::IdRole).toString(), a);
    }

    void test_tenCreations_noModelResetStorm() {
        QSignalSpy resetSpy(m_model.get(), &QAbstractItemModel::modelReset);
        QSignalSpy insertedSpy(m_model.get(), &QAbstractItemModel::rowsInserted);

        for (int i = 0; i < 10; ++i) {
            m_svc->createConversation(QStringLiteral("c%1").arg(i));
        }

        QCOMPARE(resetSpy.count(), 0);
        QCOMPARE(insertedSpy.count(), 10);
        QCOMPARE(m_model->rowCount(), 10);
    }
};

QTEST_MAIN(TestConversationListModel)
#include "test-conversation-list-model.moc"
