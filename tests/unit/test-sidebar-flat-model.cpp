// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/conversation-list-model.h"
#include "models/db-manager.h"
#include "models/sidebar-flat-model.h"
#include "services/conversation-service.h"
#include "services/membership-service.h"

#include <QTemporaryDir>
#include <QTest>

#include <QDateTime>
#include <QSignalSpy>
#include <QSqlDatabase>

class TestSidebarFlatModel : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<MembershipService> m_membershipSvc;
    std::unique_ptr<ConversationListModel> m_treeModel;
    std::unique_ptr<SidebarFlatModel> m_flat;

    void settle() { QTest::qWait(1); }

    QString rowTypeAt(int row) const {
        return m_flat->data(m_flat->index(row, 0), SidebarFlatModel::ItemTypeRole).toString();
    }
    QString rowIdAt(int row) const {
        return m_flat->data(m_flat->index(row, 0), SidebarFlatModel::ItemIdRole).toString();
    }
    QString rowTitleAt(int row) const {
        return m_flat->data(m_flat->index(row, 0), SidebarFlatModel::ItemTitleRole).toString();
    }

    int findConvRow(const QString& convId) const {
        for (int i = 0; i < m_flat->rowCount(); ++i) {
            if (rowTypeAt(i) == QStringLiteral("conversation") && rowIdAt(i) == convId) {
                return i;
            }
        }
        return -1;
    }

    int countRowType(const QString& type) const {
        int n = 0;
        for (int i = 0; i < m_flat->rowCount(); ++i) {
            if (rowTypeAt(i) == type)
                ++n;
        }
        return n;
    }

  private slots:

    void init() {
        const QString dbPath =
            m_tempDir.path() +
            QStringLiteral("/test_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_convSvc = std::make_unique<ConversationService>(DbManager::instance());
        m_membershipSvc = std::make_unique<MembershipService>(DbManager::instance());
        m_treeModel = std::make_unique<ConversationListModel>(*m_convSvc);
        m_flat = std::make_unique<SidebarFlatModel>(*m_treeModel, *m_membershipSvc);
    }

    void cleanup() {
        m_flat.reset();
        m_treeModel.reset();
        m_membershipSvc.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    }


    void test_emptyState_hasNoRows() { QCOMPARE(m_flat->rowCount(), 0); }


    void test_rootConversation_appearsUnderPlainChatsSection() {
        const QString id = m_convSvc->createConversation(QStringLiteral("A"));
        settle();
        QCOMPARE(m_flat->rowCount(), 2);
        QCOMPARE(rowTypeAt(0), QStringLiteral("section_header"));
        QCOMPARE(rowTitleAt(0), QStringLiteral("Plain Chats"));
        QCOMPARE(rowTypeAt(1), QStringLiteral("conversation"));
        QCOMPARE(rowIdAt(1), id);
    }


    void test_folderWithChildConversation_flattensInOrder() {
        const QString fid = m_convSvc->createFolder(QStringLiteral("Folder"));
        const QString cid = m_convSvc->createConversation(QStringLiteral("Child"), fid);

        settle();
        QCOMPARE(m_flat->rowCount(), 3);
        QCOMPARE(rowTypeAt(0), QStringLiteral("section_header"));
        QCOMPARE(rowTitleAt(0), QStringLiteral("Projects & Organizations"));
        QCOMPARE(rowTypeAt(1), QStringLiteral("folder"));
        QCOMPARE(rowIdAt(1), fid);
        QCOMPARE(rowTypeAt(2), QStringLiteral("conversation"));
        QCOMPARE(rowIdAt(2), cid);
    }


    void test_toggleFolder_collapsesAndRestores() {
        const QString fid = m_convSvc->createFolder(QStringLiteral("Folder"));
        m_convSvc->createConversation(QStringLiteral("Child1"), fid);
        m_convSvc->createConversation(QStringLiteral("Child2"), fid);

        settle();
        QCOMPARE(m_flat->rowCount(), 4);

        m_flat->toggleFolder(fid);
        QCOMPARE(m_flat->rowCount(), 2);
        QCOMPARE(rowTypeAt(0), QStringLiteral("section_header"));
        QCOMPARE(rowTypeAt(1), QStringLiteral("folder"));

        m_flat->toggleFolder(fid);
        QCOMPARE(m_flat->rowCount(), 4);
    }


    void test_filterText_showsOnlyMatchingConversationsInSections() {
        m_convSvc->createConversation(QStringLiteral("Apple pie"));
        m_convSvc->createConversation(QStringLiteral("Banana split"));
        m_convSvc->createConversation(QStringLiteral("Apricot tart"));

        settle();
        QCOMPARE(m_flat->rowCount(), 4);

        m_flat->setFilterText(QStringLiteral("ap"));
        QCOMPARE(m_flat->rowCount(), 3);
        QCOMPARE(rowTypeAt(0), QStringLiteral("section_header"));
        QCOMPARE(countRowType(QStringLiteral("conversation")), 2);
        for (int i = 1; i < m_flat->rowCount(); ++i) {
            QCOMPARE(rowTypeAt(i), QStringLiteral("conversation"));
            QVERIFY(rowTitleAt(i).toLower().contains(QStringLiteral("ap")));
        }

        m_flat->setFilterText(QString{});
        QCOMPARE(m_flat->rowCount(), 4);
    }


    void test_setActiveConversationId_flipsIsActiveRole() {
        const QString a = m_convSvc->createConversation(QStringLiteral("A"));
        const QString b = m_convSvc->createConversation(QStringLiteral("B"));

        settle();
        QCOMPARE(m_flat->rowCount(), 3);

        const int initialA = findConvRow(a);
        QVERIFY(initialA >= 0);
        QCOMPARE(m_flat->data(m_flat->index(initialA, 0), SidebarFlatModel::IsActiveRole).toBool(),
                 false);

        QSignalSpy dataSpy(m_flat.get(), &QAbstractItemModel::dataChanged);
        m_flat->setActiveConversationId(a);
        QVERIFY(dataSpy.count() >= 1);

        const int rowA = findConvRow(a);
        const int rowB = findConvRow(b);
        QVERIFY(rowA >= 0);
        QVERIFY(rowB >= 0);
        QCOMPARE(m_flat->data(m_flat->index(rowA, 0), SidebarFlatModel::IsActiveRole).toBool(),
                 true);
        QCOMPARE(m_flat->data(m_flat->index(rowB, 0), SidebarFlatModel::IsActiveRole).toBool(),
                 false);

        dataSpy.clear();
        m_flat->setActiveConversationId(b);
        QVERIFY(dataSpy.count() >= 1);
        QCOMPARE(m_flat->data(m_flat->index(rowA, 0), SidebarFlatModel::IsActiveRole).toBool(),
                 false);
        QCOMPARE(m_flat->data(m_flat->index(rowB, 0), SidebarFlatModel::IsActiveRole).toBool(),
                 true);
    }


    void test_burstOfCreates_coalescesIntoSingleReset() {
        QSignalSpy resetSpy(m_flat.get(), &QAbstractItemModel::modelReset);

        m_convSvc->createConversation(QStringLiteral("Burst 1"));
        m_convSvc->createConversation(QStringLiteral("Burst 2"));
        m_convSvc->createConversation(QStringLiteral("Burst 3"));

        QCOMPARE(resetSpy.count(), 0);

        settle();

        QCOMPARE(resetSpy.count(), 1);
        QCOMPARE(m_flat->rowCount(), 4);
    }
};

QTEST_MAIN(TestSidebarFlatModel)
#include "test-sidebar-flat-model.moc"
