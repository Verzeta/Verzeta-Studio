// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/agent.h"
#include "models/conversation-list-model.h"
#include "models/db-manager.h"
#include "models/sidebar-flat-model.h"
#include "services/agent-registry.h"
#include "services/conversation-controller.h"
#include "services/conversation-service.h"
#include "services/membership-service.h"
#include "services/model-router.h"

#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <QDateTime>
#include <QSqlDatabase>
#include <QString>
#include <QStringList>
#include <QUuid>

class TestSidebarSectionsModel : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<MembershipService> m_membershipSvc;
    std::unique_ptr<AgentRegistry> m_agentRegistry;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<ConversationController> m_convCtrl;
    std::unique_ptr<ConversationListModel> m_treeModel;
    std::unique_ptr<SidebarFlatModel> m_flat;

    static QString uuid() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

    QString createAgent(const QString& name) {
        Agent a;
        a.id = uuid();
        a.name = name;
        a.description = QStringLiteral("Test agent %1").arg(name);
        a.systemPrompt = QStringLiteral("You are %1.").arg(name);
        a.defaultPattern = QStringLiteral("direct");
        a.createdAt = QDateTime::currentDateTimeUtc();
        const QString id = m_agentRegistry->createAgent(a);
        Q_ASSERT(!id.isEmpty());
        return id;
    }

    QString createProjectFolder(const QString& name) {
        const QString id = m_convSvc->createFolder(name);
        m_convSvc->updateFolderMetadata(id,
                                        QStringLiteral("project"),
                                        QStringLiteral("goal"),
                                        QStringLiteral("description"),
                                        {});
        return id;
    }

    void settle() { QTest::qWait(1); }

    int findSectionRow(const QString& title) const {
        for (int i = 0; i < m_flat->rowCount(); ++i) {
            const QString t =
                m_flat->data(m_flat->index(i, 0), SidebarFlatModel::ItemTypeRole).toString();
            if (t != QStringLiteral("section_header"))
                continue;
            if (m_flat->data(m_flat->index(i, 0), SidebarFlatModel::ItemTitleRole).toString() ==
                title) {
                return i;
            }
        }
        return -1;
    }

    int findSubsectionRow(const QString& title) const {
        for (int i = 0; i < m_flat->rowCount(); ++i) {
            const QString t =
                m_flat->data(m_flat->index(i, 0), SidebarFlatModel::ItemTypeRole).toString();
            if (t != QStringLiteral("subsection_header"))
                continue;
            if (m_flat->data(m_flat->index(i, 0), SidebarFlatModel::ItemTitleRole).toString() ==
                title) {
                return i;
            }
        }
        return -1;
    }

    int countConvRowsWithId(const QString& convId) const {
        int n = 0;
        for (int i = 0; i < m_flat->rowCount(); ++i) {
            const QString t =
                m_flat->data(m_flat->index(i, 0), SidebarFlatModel::ItemTypeRole).toString();
            const QString id =
                m_flat->data(m_flat->index(i, 0), SidebarFlatModel::ItemIdRole).toString();
            if (t == QStringLiteral("conversation") && id == convId)
                ++n;
        }
        return n;
    }

    QString rowType(int row) const {
        return m_flat->data(m_flat->index(row, 0), SidebarFlatModel::ItemTypeRole).toString();
    }
    QString rowTitle(int row) const {
        return m_flat->data(m_flat->index(row, 0), SidebarFlatModel::ItemTitleRole).toString();
    }
    QString rowId(int row) const {
        return m_flat->data(m_flat->index(row, 0), SidebarFlatModel::ItemIdRole).toString();
    }
    QString rowSectionId(int row) const {
        return m_flat->data(m_flat->index(row, 0), SidebarFlatModel::SectionIdRole).toString();
    }
    bool rowSectionCollapsible(int row) const {
        return m_flat->data(m_flat->index(row, 0), SidebarFlatModel::SectionCollapsibleRole)
            .toBool();
    }
    bool rowIsPinned(int row) const {
        return m_flat->data(m_flat->index(row, 0), SidebarFlatModel::IsPinnedRole).toBool();
    }
    int rowDepth(int row) const {
        return m_flat->data(m_flat->index(row, 0), SidebarFlatModel::DepthRole).toInt();
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
        m_agentRegistry = std::make_unique<AgentRegistry>(DbManager::instance());
        m_agentRegistry->initialize();
        m_router = std::make_unique<ModelRouter>();
        m_convCtrl = std::make_unique<ConversationController>(*m_convSvc, *m_router);
        m_convCtrl->setMembershipService(m_membershipSvc.get());
        m_treeModel = std::make_unique<ConversationListModel>(*m_convSvc);
        m_flat = std::make_unique<SidebarFlatModel>(*m_treeModel, *m_membershipSvc);
    }

    void cleanup() {
        m_flat.reset();
        m_treeModel.reset();
        m_convCtrl.reset();
        m_router.reset();
        m_agentRegistry.reset();
        m_membershipSvc.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    }


    void test_sectionHeader_isCollapsibleWithStableId() {
        m_convSvc->createConversation(QStringLiteral("Hello"));
        settle();
        const int row = findSectionRow(QStringLiteral("Plain Chats"));
        QVERIFY(row >= 0);
        QVERIFY(rowSectionCollapsible(row));
        QVERIFY(rowSectionId(row).startsWith(QStringLiteral("section:")));
        QCOMPARE(rowDepth(row), 0);
    }


    void test_rootConvNoAgentNoGroup_landsInPlainChats() {
        const QString id = m_convSvc->createConversation(QStringLiteral("Plain"));
        settle();
        const int section = findSectionRow(QStringLiteral("Plain Chats"));
        QVERIFY(section >= 0);
        QCOMPARE(rowType(section + 1), QStringLiteral("conversation"));
        QCOMPARE(rowId(section + 1), id);
        QCOMPARE(findSectionRow(QStringLiteral("Direct Agent Chats")), -1);
        QCOMPARE(findSectionRow(QStringLiteral("Standalone Group Chats")), -1);
        QCOMPARE(findSectionRow(QStringLiteral("Pinned")), -1);
    }


    void test_rootConvWithPrimaryAgent_landsInDirectAgentChats() {
        const QString agentId = createAgent(QStringLiteral("Solo"));
        const QString convId =
            m_convCtrl->openDirectChatWithMember(QString(), agentId, QStringLiteral("Solo"));
        QVERIFY(!convId.isEmpty());
        settle();
        QCOMPARE(findSectionRow(QStringLiteral("Direct Agent Chats")) >= 0, true);
        QCOMPARE(findSectionRow(QStringLiteral("Plain Chats")), -1);
    }


    void test_rootGroupConv_landsInStandaloneGroupChats() {
        const QString agentA = createAgent(QStringLiteral("A"));
        const QString agentB = createAgent(QStringLiteral("B"));
        const QString convId = m_convSvc->createGroupConversation(
            QStringLiteral("Stand-Alone Group"), {agentA, agentB}, QString());
        QVERIFY(!convId.isEmpty());
        settle();
        QCOMPARE(findSectionRow(QStringLiteral("Standalone Group Chats")) >= 0, true);
        QCOMPARE(findSectionRow(QStringLiteral("Plain Chats")), -1);
    }


    void test_projectFolder_splitsConvsIntoGroupAndDirectSubsections() {
        const QString folderId = createProjectFolder(QStringLiteral("Acme"));
        const QString agentA = createAgent(QStringLiteral("Alice"));
        const QString agentB = createAgent(QStringLiteral("Bob"));
        m_membershipSvc->addProjectMember(folderId, agentA, QStringLiteral("Alice"), false);
        m_membershipSvc->addProjectMember(folderId, agentB, QStringLiteral("Bob"), false);

        const QString directId =
            m_convCtrl->openDirectChatWithMember(folderId, agentA, QStringLiteral("Alice"));
        QVERIFY(!directId.isEmpty());

        const QString groupId = m_convSvc->createGroupConversation(
            QStringLiteral("Project Standup"), {agentA, agentB}, folderId);
        QVERIFY(!groupId.isEmpty());

        settle();
        const int gRow = findSubsectionRow(QStringLiteral("Group Chats"));
        const int dRow = findSubsectionRow(QStringLiteral("Direct Chats"));
        QVERIFY2(gRow >= 0, "Group Chats subsection missing");
        QVERIFY2(dRow >= 0, "Direct Chats subsection missing");

        for (int i = 0; i < m_flat->rowCount(); ++i) {
            QVERIFY(rowType(i) != QStringLiteral("convsHeader"));
        }

        QVERIFY(!rowSectionCollapsible(gRow));
        QVERIFY(!rowSectionCollapsible(dRow));
    }


    void test_projectDirectChat_displaysDerivedAliasAndAgentName() {
        const QString folderId = createProjectFolder(QStringLiteral("ProjectAlpha"));
        const QString agentId = createAgent(QStringLiteral("Test Senior PM"));
        m_membershipSvc->addProjectMember(folderId, agentId, QStringLiteral("Alice"), false);

        const QString convId =
            m_convCtrl->openDirectChatWithMember(folderId, agentId, QStringLiteral("Alice"));
        QVERIFY(!convId.isEmpty());

        settle();
        int convRow = -1;
        for (int i = 0; i < m_flat->rowCount(); ++i) {
            if (rowType(i) == QStringLiteral("conversation") && rowId(i) == convId) {
                convRow = i;
                break;
            }
        }
        QVERIFY(convRow >= 0);
        QCOMPARE(rowTitle(convRow), QStringLiteral("Alice (Test Senior PM)"));
    }


    void test_pinnedConv_appearsInPinnedAndNaturalSection() {
        const QString id = m_convSvc->createConversation(QStringLiteral("Pin Me"));
        QVERIFY(m_convSvc->setConversationPinned(id, true));

        settle();
        QCOMPARE(countConvRowsWithId(id), 2);

        const int pinnedRow = findSectionRow(QStringLiteral("Pinned"));
        const int plainRow = findSectionRow(QStringLiteral("Plain Chats"));
        QVERIFY(pinnedRow >= 0);
        QVERIFY(plainRow >= 0);
        QVERIFY(pinnedRow < plainRow);

        for (int i = 0; i < m_flat->rowCount(); ++i) {
            if (rowType(i) == QStringLiteral("conversation") && rowId(i) == id) {
                QCOMPARE(rowIsPinned(i), true);
            }
        }
    }

    void test_unpinnedConv_doesNotAppearInPinnedSection() {
        const QString id = m_convSvc->createConversation(QStringLiteral("Unpin Me"));
        QVERIFY(m_convSvc->setConversationPinned(id, true));
        settle();
        QCOMPARE(countConvRowsWithId(id), 2);

        QVERIFY(m_convSvc->setConversationPinned(id, false));
        settle();
        QCOMPARE(countConvRowsWithId(id), 1);
        QCOMPARE(findSectionRow(QStringLiteral("Pinned")), -1);
    }


    void test_filter_hidesSectionsWithZeroMatches() {
        m_convSvc->createConversation(QStringLiteral("Apple plain"));
        const QString agentId = createAgent(QStringLiteral("X"));
        m_convCtrl->openDirectChatWithMember(QString(), agentId, QStringLiteral("X"));

        settle();
        QVERIFY(findSectionRow(QStringLiteral("Plain Chats")) >= 0);
        QVERIFY(findSectionRow(QStringLiteral("Direct Agent Chats")) >= 0);

        m_flat->setFilterText(QStringLiteral("apple"));
        QVERIFY(findSectionRow(QStringLiteral("Plain Chats")) >= 0);
        QCOMPARE(findSectionRow(QStringLiteral("Direct Agent Chats")), -1);

        m_flat->setFilterText(QString());
        QVERIFY(findSectionRow(QStringLiteral("Direct Agent Chats")) >= 0);
    }


    void test_toggleSection_hidesContentKeepsHeader() {
        m_convSvc->createConversation(QStringLiteral("Plain1"));
        m_convSvc->createConversation(QStringLiteral("Plain2"));

        settle();
        const int rowsBefore = m_flat->rowCount();
        QCOMPARE(rowsBefore, 3);

        const int sec = findSectionRow(QStringLiteral("Plain Chats"));
        QVERIFY(sec >= 0);
        const QString sid = rowSectionId(sec);

        QVERIFY(!m_flat->isSectionCollapsed(sid));
        m_flat->toggleSection(sid);
        QVERIFY(m_flat->isSectionCollapsed(sid));
        QCOMPARE(m_flat->rowCount(), 1);
        QCOMPARE(rowType(0), QStringLiteral("section_header"));

        m_flat->toggleSection(sid);
        QVERIFY(!m_flat->isSectionCollapsed(sid));
        QCOMPARE(m_flat->rowCount(), 3);
    }

    void test_setSectionCollapsed_isIdempotent() {
        m_convSvc->createConversation(QStringLiteral("Plain"));
        settle();
        const int sec = findSectionRow(QStringLiteral("Plain Chats"));
        const QString sid = rowSectionId(sec);

        m_flat->setSectionCollapsed(sid, true);
        QVERIFY(m_flat->isSectionCollapsed(sid));
        m_flat->setSectionCollapsed(sid, true);
        QVERIFY(m_flat->isSectionCollapsed(sid));

        m_flat->setSectionCollapsed(sid, false);
        QVERIFY(!m_flat->isSectionCollapsed(sid));
    }

    void test_emptySectionId_isNoOp() {
        m_convSvc->createConversation(QStringLiteral("Plain"));
        settle();
        QCOMPARE(m_flat->rowCount(), 2);

        m_flat->toggleSection(QString());
        m_flat->setSectionCollapsed(QString(), true);
        QVERIFY(!m_flat->isSectionCollapsed(QString()));
        QCOMPARE(m_flat->rowCount(), 2);
    }


    void test_sectionContents_sortByUpdatedAtDescending() {
        const QString a = m_convSvc->createConversation(QStringLiteral("First"));
        QTest::qWait(2);
        const QString b = m_convSvc->createConversation(QStringLiteral("Second"));
        QTest::qWait(2);
        const QString c = m_convSvc->createConversation(QStringLiteral("Third"));

        settle();
        QCOMPARE(rowType(0), QStringLiteral("section_header"));
        QCOMPARE(rowId(1), c);
        QCOMPARE(rowId(2), b);
        QCOMPARE(rowId(3), a);
    }


    void test_emptyDb_emitsNoRows() { QCOMPARE(m_flat->rowCount(), 0); }
};

QTEST_MAIN(TestSidebarSectionsModel)
#include "test-sidebar-sections-model.moc"
