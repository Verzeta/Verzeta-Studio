// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file sidebar-flat-model.cpp
 * @brief Implementation of the push-based sidebar flat model.
 *
 *        Extends the original folder-tree flatten with five top-level
 *        collapsible sections (Pinned / Projects & Organizations /
 *        Standalone Group Chats / Direct Agent Chats / Plain Chats) and
 *        per-project Group Chats / Direct Chats subsections.
 * @layer Service (Model)
 * @dependencies ConversationListModel, MembershipService, Qt6::Core.
 */


#include "sidebar-flat-model.h"

#include "../models/conversation-list-model.h"
#include "../models/member.h"
#include "../services/membership-service.h"
#include "../utils/logger.h"

#include <algorithm>
#include <QDateTime>
#include <QRegularExpression>

namespace {

constexpr int kIdRole = ConversationListModel::IdRole;
constexpr int kTitleRole = ConversationListModel::TitleRole;
constexpr int kTypeRole = ConversationListModel::TypeRole;
constexpr int kUpdatedAtRole = ConversationListModel::UpdatedAtRole;
constexpr int kFolderTypeRole = ConversationListModel::FolderTypeRole;
constexpr int kIsPinnedRole = ConversationListModel::IsPinnedRole;
constexpr int kIsGroupRole = ConversationListModel::IsGroupRole;
constexpr int kPrimaryAgentIdRole = ConversationListModel::PrimaryAgentIdRole;

// Conversation tree nodes don't expose primary_agent_id directly through
// the existing role set — categorisation needs it to bucket convs into
// "direct agent" vs "plain". Pull from ConversationService via id below.
}  // namespace

// ---------------------------------------------------------------------------

SidebarFlatModel::SidebarFlatModel(ConversationListModel& tree,
                                   MembershipService& membership,
                                   QObject* parent)
    : QAbstractListModel(parent), m_tree(tree), m_membership(membership) {
    // Subscribe to every upstream mutation signal the tree emits.
    // The tree's own row-level signals would in principle let us do
    // per-row delta updates on the flat model, but the flatten logic
    // is order-sensitive across heterogeneous row types (members,
    // headers, conversations) so we rebuild fully on any structural
    // change. One rebuild is O(total_folders + total_convs); a burst of
    // upstream signals is coalesced into a single rebuild via
    // m_rebuildTimer (see scheduleRebuild) so one user action — e.g.
    // creating a project room — never triggers dozens of resets.
    connect(&m_tree, &QAbstractItemModel::modelReset, this, &SidebarFlatModel::onUpstreamChanged);
    connect(&m_tree, &QAbstractItemModel::rowsInserted, this, &SidebarFlatModel::onUpstreamChanged);
    connect(&m_tree, &QAbstractItemModel::rowsRemoved, this, &SidebarFlatModel::onUpstreamChanged);
    connect(&m_tree, &QAbstractItemModel::rowsMoved, this, &SidebarFlatModel::onUpstreamChanged);
    connect(&m_tree, &QAbstractItemModel::dataChanged, this, &SidebarFlatModel::onUpstreamChanged);

    connect(&m_membership,
            &MembershipService::projectMembersChanged,
            this,
            &SidebarFlatModel::onProjectMembersChanged);
    connect(&m_membership,
            &MembershipService::conversationMembersChanged,
            this,
            &SidebarFlatModel::onConversationMembersChanged);

    // Coalesce rebuilds onto a single-shot 0 ms timer. A whole burst of
    // upstream signals fired within one synchronous call chain collapses
    // into exactly one rebuild on the next event-loop pass instead of
    // one O(n) reset each. The initial build below stays synchronous so
    // the model is populated before its first read.
    m_rebuildTimer.setSingleShot(true);
    m_rebuildTimer.setInterval(0);
    connect(&m_rebuildTimer, &QTimer::timeout, this, &SidebarFlatModel::rebuild);

    rebuild();
}

int SidebarFlatModel::count() const {
    return static_cast<int>(m_rows.size());
}

int SidebarFlatModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return static_cast<int>(m_rows.size());
}

QVariant SidebarFlatModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(m_rows.size())) {
        return {};
    }
    const Row& r = m_rows.at(index.row());
    switch (role) {
        case ItemTypeRole:
            return r.itemType;
        case ItemIdRole:
            return r.itemId;
        case ItemTitleRole:
            return r.itemTitle;
        case ItemSubtitleRole:
            return r.itemSubtitle;
        case ItemChildCountRole:
            return r.itemChildCount;
        case FolderKindRole:
            return r.folderKind;
        case FolderExpandedRole:
            return r.folderExpanded;
        case MemberAliasRole:
            return r.memberAlias;
        case MemberAgentIdRole:
            return r.memberAgentId;
        case MemberAgentNameRole:
            return r.memberAgentName;
        case MemberIsCoordinatorRole:
            return r.memberIsCoordinator;
        case MemberIconNameRole:
            return r.memberIconName;
        case ParentFolderIdRole:
            return r.parentFolderId;
        case IsActiveRole:
            return r.itemType == QStringLiteral("conversation") &&
                   !m_activeConversationId.isEmpty() && r.itemId == m_activeConversationId;
        case IsPinnedRole:
            return r.isPinned;
        case SectionIdRole:
            return r.sectionId;
        case SectionCollapsibleRole:
            return r.sectionCollapsible;
        case SectionCollapsedRole:
            return r.sectionCollapsed;
        case DepthRole:
            return r.depth;
        default:
            return {};
    }
}

QHash<int, QByteArray> SidebarFlatModel::roleNames() const {
    return {
        {ItemTypeRole, QByteArrayLiteral("itemType")},
        {ItemIdRole, QByteArrayLiteral("itemId")},
        {ItemTitleRole, QByteArrayLiteral("itemTitle")},
        {ItemSubtitleRole, QByteArrayLiteral("itemSubtitle")},
        {ItemChildCountRole, QByteArrayLiteral("itemChildCount")},
        {FolderKindRole, QByteArrayLiteral("folderKind")},
        {FolderExpandedRole, QByteArrayLiteral("folderExpanded")},
        {MemberAliasRole, QByteArrayLiteral("memberAlias")},
        {MemberAgentIdRole, QByteArrayLiteral("memberAgentId")},
        {MemberAgentNameRole, QByteArrayLiteral("memberAgentName")},
        {MemberIsCoordinatorRole, QByteArrayLiteral("memberIsCoordinator")},
        {MemberIconNameRole, QByteArrayLiteral("memberIconName")},
        {ParentFolderIdRole, QByteArrayLiteral("parentFolderId")},
        {IsActiveRole, QByteArrayLiteral("isActive")},
        {IsPinnedRole, QByteArrayLiteral("isPinned")},
        {SectionIdRole, QByteArrayLiteral("sectionId")},
        {SectionCollapsibleRole, QByteArrayLiteral("sectionCollapsible")},
        {SectionCollapsedRole, QByteArrayLiteral("sectionCollapsed")},
        {DepthRole, QByteArrayLiteral("depth")},
    };
}


bool SidebarFlatModel::isSectionCollapsed(const QString& sectionId) const {
    return !sectionId.isEmpty() && m_collapsedSections.contains(sectionId);
}

void SidebarFlatModel::toggleSection(const QString& sectionId) {
    if (sectionId.isEmpty())
        return;
    if (m_collapsedSections.contains(sectionId)) {
        m_collapsedSections.remove(sectionId);
    } else {
        m_collapsedSections.insert(sectionId);
    }
    rebuild();
}

void SidebarFlatModel::setSectionCollapsed(const QString& sectionId, bool collapsed) {
    if (sectionId.isEmpty())
        return;
    const bool wasCollapsed = m_collapsedSections.contains(sectionId);
    if (collapsed == wasCollapsed)
        return;
    if (collapsed)
        m_collapsedSections.insert(sectionId);
    else
        m_collapsedSections.remove(sectionId);
    rebuild();
}

QString SidebarFlatModel::activeConversationId() const {
    return m_activeConversationId;
}

void SidebarFlatModel::setActiveConversationId(const QString& convId) {
    if (convId == m_activeConversationId)
        return;

    const QString previousId = m_activeConversationId;
    m_activeConversationId = convId;

    // Emit dataChanged(IsActiveRole) for the row that was previously
    // active and the row that is now active. Both rows may or may not
    // be in the current flat view (the new conversation might be in a
    // collapsed folder, or behind a search filter that hides it).
    auto emitForId = [this](const QString& id) {
        if (id.isEmpty())
            return;
        for (int i = 0; i < m_rows.size(); ++i) {
            if (m_rows.at(i).itemType == QStringLiteral("conversation") &&
                m_rows.at(i).itemId == id) {
                const QModelIndex idx = index(i, 0);
                emit dataChanged(idx, idx, {IsActiveRole});
                return;
            }
        }
    };
    emitForId(previousId);
    emitForId(convId);
}

bool SidebarFlatModel::isFolderExpanded(const QString& folderId) const {
    return !m_collapsedFolders.contains(folderId);
}

void SidebarFlatModel::toggleFolder(const QString& folderId) {
    if (m_collapsedFolders.contains(folderId)) {
        m_collapsedFolders.remove(folderId);
    } else {
        m_collapsedFolders.insert(folderId);
    }
    rebuild();
}

QString SidebarFlatModel::filterText() const {
    return m_filterText;
}

void SidebarFlatModel::setFilterText(const QString& text) {
    const QString normalized = text.trimmed();
    if (normalized == m_filterText)
        return;
    m_filterText = normalized;
    rebuild();
    emit filterTextChanged();
}

// ---------------------------------------------------------------------------
// Signal handlers
// ---------------------------------------------------------------------------

void SidebarFlatModel::onUpstreamChanged() {
    scheduleRebuild();
}

void SidebarFlatModel::onProjectMembersChanged(const QString& /*folderId*/) {
    scheduleRebuild();
}

void SidebarFlatModel::onConversationMembersChanged(const QString& /*convId*/) {
    scheduleRebuild();
}

void SidebarFlatModel::scheduleRebuild() {
    // Coalesce: if a rebuild is already armed for the next event-loop
    // pass, this signal folds into it. The isActive() guard means a
    // burst of N signals arms the timer once, not N times.
    if (!m_rebuildTimer.isActive()) {
        m_rebuildTimer.start();
    }
}


void SidebarFlatModel::rebuild() {
    m_rebuildTimer.stop();

    beginResetModel();
    m_rows.clear();

    const QList<ConvCategorised> all = gatherCategorisedConversations();

    // -----------------------------------------------------------------
    // Pinned section — every is_pinned conv, regardless of folder.
    // -----------------------------------------------------------------
    {
        QList<ConvCategorised> pinned;
        for (const ConvCategorised& c : all) {
            if (c.isPinned && matchesFilter(c))
                pinned.append(c);
        }
        std::stable_sort(
            pinned.begin(), pinned.end(), [](const ConvCategorised& a, const ConvCategorised& b) {
                return a.updatedAt > b.updatedAt;
            });
        if (!pinned.isEmpty()) {
            const QString id(QString::fromLatin1(kSectionPinned));
            appendSectionHeader(id, tr("Pinned"), pinned.size());
            if (!isSectionCollapsed(id)) {
                for (const ConvCategorised& c : pinned) {
                    appendConversationRow(c, /*depth=*/1);
                }
            }
        }
    }

    // -----------------------------------------------------------------
    // Projects & Organizations — wraps the existing folder tree.
    //
    // Folders surface in the tree's natural top-level order (matching
    // ConversationListModel::buildTree which lists folders before root
    // conversations, name ASC). Inside project/org folder bodies the
    // member rows + Start Group Chat action come from MembershipService
    // and the convs split into Group / Direct subsections.
    // -----------------------------------------------------------------
    {
        // Collect folder indices first so the section header's count
        // reflects the number of folders, not member rows or convs.
        /**
         * @brief One folder gathered from the upstream tree. Captures
         *        the bits we need without holding a persistent QModelIndex
         *        across rebuild iterations.
         */
        struct TreeFolder {
            QString folderId;
            QString title;
            QString kind;
            QModelIndex idx;
        };
        QList<TreeFolder> folders;
        const int rootCount = m_tree.rowCount();
        for (int i = 0; i < rootCount; ++i) {
            const QModelIndex idx = m_tree.index(i, 0);
            const QString type = m_tree.data(idx, kTypeRole).toString();
            if (type != QStringLiteral("folder"))
                continue;
            TreeFolder f;
            f.folderId = m_tree.data(idx, kIdRole).toString();
            f.title = m_tree.data(idx, kTitleRole).toString();
            f.kind = m_tree.data(idx, kFolderTypeRole).toString();
            if (f.kind.isEmpty())
                f.kind = QStringLiteral("regular");
            f.idx = idx;
            folders.append(f);
        }

        // For filter mode: a folder is "visible" only if at least one of
        // its convs matches the filter (member rows / start-group-chat
        // links don't count — search is title-only on conversations).
        // Collect filtered convs per folder up-front so we can decide
        // visibility AND emit them without a second pass.
        /**
         * @brief Per-folder bucket of post-filter conversations. Cached
         *        so we don't walk the upstream tree twice per rebuild.
         */
        struct FolderConvs {
            QList<ConvCategorised> all;  // post-filter, sort by updated_at DESC.
        };
        QHash<QString, FolderConvs> perFolder;
        for (const ConvCategorised& c : all) {
            if (c.parentFolderId.isEmpty())
                continue;
            if (!matchesFilter(c))
                continue;
            perFolder[c.parentFolderId].all.append(c);
        }
        for (auto it = perFolder.begin(); it != perFolder.end(); ++it) {
            std::stable_sort(it->all.begin(),
                             it->all.end(),
                             [](const ConvCategorised& a, const ConvCategorised& b) {
                                 return a.updatedAt > b.updatedAt;
                             });
        }

        // When a filter is active, hide folders that have NO matching
        // conversations. When no filter is active, every folder appears.
        QList<TreeFolder> visibleFolders;
        for (const TreeFolder& f : folders) {
            if (m_filterText.isEmpty()) {
                visibleFolders.append(f);
            } else if (perFolder.contains(f.folderId) && !perFolder[f.folderId].all.isEmpty()) {
                visibleFolders.append(f);
            }
        }

        if (!visibleFolders.isEmpty()) {
            const QString id(QString::fromLatin1(kSectionProjectsOrgs));
            appendSectionHeader(id, tr("Projects & Organizations"), visibleFolders.size());
            if (!isSectionCollapsed(id)) {
                for (const TreeFolder& f : visibleFolders) {
                    appendFolderRows(
                        f.folderId, f.title, f.kind, perFolder.value(f.folderId).all, f.idx);
                }
            }
        }
    }

    // -----------------------------------------------------------------
    // Standalone Group Chats — root convs with isGroup = true.
    // -----------------------------------------------------------------
    {
        QList<ConvCategorised> bucket;
        for (const ConvCategorised& c : all) {
            if (!c.parentFolderId.isEmpty())
                continue;
            if (!c.isGroup)
                continue;
            if (!matchesFilter(c))
                continue;
            bucket.append(c);
        }
        std::stable_sort(
            bucket.begin(), bucket.end(), [](const ConvCategorised& a, const ConvCategorised& b) {
                return a.updatedAt > b.updatedAt;
            });
        if (!bucket.isEmpty()) {
            const QString id(QString::fromLatin1(kSectionStandaloneGrp));
            appendSectionHeader(id, tr("Standalone Group Chats"), bucket.size());
            if (!isSectionCollapsed(id)) {
                for (const ConvCategorised& c : bucket) {
                    appendConversationRow(c, /*depth=*/1);
                }
            }
        }
    }

    // -----------------------------------------------------------------
    // Direct Agent Chats — root convs with !isGroup AND primary_agent_id.
    // -----------------------------------------------------------------
    {
        QList<ConvCategorised> bucket;
        for (const ConvCategorised& c : all) {
            if (!c.parentFolderId.isEmpty())
                continue;
            if (c.isGroup)
                continue;
            if (c.primaryAgentId.isEmpty())
                continue;
            if (!matchesFilter(c))
                continue;
            bucket.append(c);
        }
        std::stable_sort(
            bucket.begin(), bucket.end(), [](const ConvCategorised& a, const ConvCategorised& b) {
                return a.updatedAt > b.updatedAt;
            });
        if (!bucket.isEmpty()) {
            const QString id(QString::fromLatin1(kSectionDirectAgents));
            appendSectionHeader(id, tr("Direct Agent Chats"), bucket.size());
            if (!isSectionCollapsed(id)) {
                for (const ConvCategorised& c : bucket) {
                    appendConversationRow(c, /*depth=*/1);
                }
            }
        }
    }

    // -----------------------------------------------------------------
    // Plain Chats — root convs with !isGroup AND no primary_agent_id.
    // -----------------------------------------------------------------
    {
        QList<ConvCategorised> bucket;
        for (const ConvCategorised& c : all) {
            if (!c.parentFolderId.isEmpty())
                continue;
            if (c.isGroup)
                continue;
            if (!c.primaryAgentId.isEmpty())
                continue;
            if (!matchesFilter(c))
                continue;
            bucket.append(c);
        }
        std::stable_sort(
            bucket.begin(), bucket.end(), [](const ConvCategorised& a, const ConvCategorised& b) {
                return a.updatedAt > b.updatedAt;
            });
        if (!bucket.isEmpty()) {
            const QString id(QString::fromLatin1(kSectionPlainChats));
            appendSectionHeader(id, tr("Plain Chats"), bucket.size());
            if (!isSectionCollapsed(id)) {
                for (const ConvCategorised& c : bucket) {
                    appendConversationRow(c, /*depth=*/1);
                }
            }
        }
    }

    endResetModel();
    emit countChanged();
}

// ---------------------------------------------------------------------------
// Categoriser — gather every conversation (folder + root) once.
// ---------------------------------------------------------------------------

QList<SidebarFlatModel::ConvCategorised> SidebarFlatModel::gatherCategorisedConversations() const {
    QList<ConvCategorised> out;
    const int rootCount = m_tree.rowCount();
    out.reserve(64);

    auto fromIndex = [&](const QModelIndex& idx, const QString& parentFolderId) -> ConvCategorised {
        ConvCategorised c;
        c.id = m_tree.data(idx, kIdRole).toString();
        c.title = m_tree.data(idx, kTitleRole).toString();
        c.subtitle = m_tree.data(idx, kUpdatedAtRole).toString();
        c.parentFolderId = parentFolderId;
        c.primaryAgentId = m_tree.data(idx, kPrimaryAgentIdRole).toString();
        c.isGroup = m_tree.data(idx, kIsGroupRole).toBool();
        c.isPinned = m_tree.data(idx, kIsPinnedRole).toBool();
        // updated_at as ISO-string is what the model exposes; parse back
        // to QDateTime for stable sort. Empty strings fall back to a
        // far-past datetime so the bucket sort is total.
        c.updatedAt = QDateTime::fromString(c.subtitle, Qt::ISODate);
        if (!c.updatedAt.isValid()) {
            c.updatedAt = QDateTime::fromMSecsSinceEpoch(0);
        }
        return c;
    };

    for (int i = 0; i < rootCount; ++i) {
        const QModelIndex idx = m_tree.index(i, 0);
        const QString type = m_tree.data(idx, kTypeRole).toString();
        if (type == QStringLiteral("folder")) {
            const QString folderId = m_tree.data(idx, kIdRole).toString();
            const int childCount = m_tree.rowCount(idx);
            for (int j = 0; j < childCount; ++j) {
                const QModelIndex childIdx = m_tree.index(j, 0, idx);
                out.append(fromIndex(childIdx, folderId));
            }
        } else {
            out.append(fromIndex(idx, /*parentFolderId=*/QString()));
        }
    }
    return out;
}

bool SidebarFlatModel::matchesFilter(const ConvCategorised& c) const {
    if (m_filterText.isEmpty())
        return true;
    return c.title.toLower().contains(m_filterText.toLower());
}

// ---------------------------------------------------------------------------
// Row-emission helpers
// ---------------------------------------------------------------------------

void SidebarFlatModel::appendSectionHeader(const QString& sectionId,
                                           const QString& title,
                                           int count) {
    Row r;
    r.itemType = QStringLiteral("section_header");
    r.itemId = sectionId;
    r.itemTitle = title;
    r.itemChildCount = count;
    r.sectionId = sectionId;
    r.sectionCollapsible = true;
    r.sectionCollapsed = m_collapsedSections.contains(sectionId);
    r.depth = 0;
    m_rows.append(r);
}

void SidebarFlatModel::appendSubsectionHeader(const QString& sectionId,
                                              const QString& title,
                                              int count,
                                              const QString& parentFolderId,
                                              int depth) {
    Row r;
    r.itemType = QStringLiteral("subsection_header");
    r.itemId = sectionId;
    r.itemTitle = title;
    r.itemChildCount = count;
    r.sectionId = sectionId;
    r.sectionCollapsible = false;  // always-expanded
    r.sectionCollapsed = false;
    r.parentFolderId = parentFolderId;
    r.depth = depth;
    m_rows.append(r);
}

void SidebarFlatModel::appendConversationRow(const ConvCategorised& c,
                                             int depth,
                                             const QString& overrideTitle) {
    Row r;
    r.itemType = QStringLiteral("conversation");
    r.itemId = c.id;
    r.itemTitle = overrideTitle.isEmpty() ? c.title : overrideTitle;
    r.itemSubtitle = c.subtitle;
    r.parentFolderId = c.parentFolderId;
    r.isPinned = c.isPinned;
    r.depth = depth;
    m_rows.append(r);
}

QString SidebarFlatModel::derivedDirectChatTitle(const QString& folderId,
                                                 const QString& title) const {
    // The natural title from openDirectChatWithMember is "Chat with @<alias>".
    // Strip the prefix and append the agent's name in parens for the
    // project's Direct Chats subsection — e.g. "Alice (Project Manager)".
    static const QRegularExpression rx(QStringLiteral("^Chat with @(.+)$"));
    const auto m = rx.match(title);
    if (!m.hasMatch())
        return title;

    const QString aliasRaw = m.captured(1);
    if (aliasRaw.isEmpty() || folderId.isEmpty())
        return title;

    // Membership lookup is case-insensitive on alias to match the
    // openDirectChatWithMember reuse predicate.
    const QList<Member> members = m_membership.projectMembers(folderId);
    for (const Member& mem : members) {
        if (mem.alias.compare(aliasRaw, Qt::CaseInsensitive) == 0) {
            if (mem.agentName.isEmpty())
                return aliasRaw;
            return QStringLiteral("%1 (%2)").arg(aliasRaw, mem.agentName);
        }
    }
    return aliasRaw;
}

void SidebarFlatModel::appendFolderRows(const QString& folderId,
                                        const QString& folderTitle,
                                        const QString& folderKind,
                                        const QList<ConvCategorised>& folderConvs,
                                        const QModelIndex& folderIdx) {
    Q_UNUSED(folderIdx);
    const bool expanded = isFolderExpanded(folderId);

    // Folder header row — depth 1 (it lives inside the
    // "Projects & Organizations" section header at depth 0).
    {
        Row r;
        r.itemType = QStringLiteral("folder");
        r.itemId = folderId;
        r.itemTitle = folderTitle;
        r.itemChildCount = folderConvs.size();
        r.folderKind = folderKind;
        r.folderExpanded = expanded;
        r.depth = 1;
        m_rows.append(r);
    }

    if (!expanded)
        return;

    const bool isProjectish =
        folderKind == QStringLiteral("project") || folderKind == QStringLiteral("organization");

    // Project/organization folders: members + start-group-chat row.
    if (isProjectish) {
        const QList<Member> members = m_membership.projectMembers(folderId);
        if (!members.isEmpty()) {
            Row header;
            header.itemType = QStringLiteral("membersHeader");
            header.itemId = folderId + QStringLiteral("::__membersHeader");
            header.itemTitle = tr("TEAM MEMBERS");
            header.folderKind = folderKind;
            header.parentFolderId = folderId;
            header.itemChildCount = members.size();
            header.depth = 2;
            m_rows.append(header);

            for (const Member& m : members) {
                Row r;
                r.itemType = QStringLiteral("member");
                r.itemId = folderId + QStringLiteral("::") + m.alias;
                r.itemTitle = m.alias;
                r.folderKind = folderKind;
                r.parentFolderId = folderId;
                r.memberAlias = m.alias;
                r.memberAgentId = m.agentId;
                r.memberAgentName = m.agentName;
                r.memberIsCoordinator = m.isCoordinator;
                r.memberIconName =
                    m.agentIconName.isEmpty() ? QStringLiteral("face-smile") : m.agentIconName;
                r.depth = 2;
                m_rows.append(r);
            }

            if (members.size() >= 2) {
                Row action;
                action.itemType = QStringLiteral("groupChatAction");
                action.itemId = folderId + QStringLiteral("::__groupChatAction");
                action.itemTitle = tr("Start Group Chat with All");
                action.folderKind = folderKind;
                action.parentFolderId = folderId;
                action.depth = 2;
                m_rows.append(action);
            }
        }

        QList<ConvCategorised> groupConvs;
        QList<ConvCategorised> directConvs;
        for (const ConvCategorised& c : folderConvs) {
            if (c.isGroup)
                groupConvs.append(c);
            else
                directConvs.append(c);
        }

        if (!groupConvs.isEmpty()) {
            const QString sid = folderId + QStringLiteral("::__groupChatsSubsection");
            appendSubsectionHeader(
                sid, tr("Group Chats"), groupConvs.size(), folderId, /*depth=*/2);
            for (const ConvCategorised& c : groupConvs) {
                appendConversationRow(c, /*depth=*/3);
            }
        }
        if (!directConvs.isEmpty()) {
            const QString sid = folderId + QStringLiteral("::__directChatsSubsection");
            appendSubsectionHeader(
                sid, tr("Direct Chats"), directConvs.size(), folderId, /*depth=*/2);
            for (const ConvCategorised& c : directConvs) {
                const QString derived = derivedDirectChatTitle(folderId, c.title);
                appendConversationRow(c, /*depth=*/3, derived);
            }
        }
        return;
    }

    for (const ConvCategorised& c : folderConvs) {
        appendConversationRow(c, /*depth=*/2);
    }
}
