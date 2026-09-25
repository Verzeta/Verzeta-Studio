// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file sidebar-flat-model.h
 * @brief Flat model that transforms the hierarchical ConversationListModel
 *        + MembershipService into a single QAbstractListModel row stream
 *        suitable for a QML ListView.
 *
 *        Row types surfaced:
 *          - "section_header":     top-level collapsible section
 *                                   (Pinned / Projects & Organizations /
 *                                    Standalone Group Chats /
 *                                    Direct Agent Chats / Plain Chats).
 *          - "folder":             folder header row (expand/collapse state)
 *          - "membersHeader":      project/org "TEAM MEMBERS" section label
 *          - "member":             a project member row (alias + role)
 *          - "groupChatAction":    "Start Group Chat with All" action row
 *          - "subsection_header":  project-folder "Group Chats" /
 *                                   "Direct Chats" subsection label.
 *                                   Always-expanded with count; replaces
 *                                   the legacy "convsHeader" inside
 *                                   project/org folders.
 *          - "conversation":       a conversation row
 *
 *        All row changes flow from signals on the upstream services:
 *          - ConversationListModel::{rowsInserted,rowsRemoved,rowsMoved,dataChanged,modelReset}
 *          - MembershipService::{projectMembersChanged,conversationMembersChanged}
 *
 *        There is NO refresh() method exposed to QML. The flat model
 *        re-derives its row list on relevant upstream signals and issues
 *        its own beginResetModel() around the derivation. A burst of
 *        upstream signals is coalesced into ONE rebuild via a single-shot
 *        timer (see scheduleRebuild) so a single user action never
 *        triggers dozens of full resets.
 *
 *        Expansion state is owned here via:
 *          - QSet<QString> m_collapsedFolders   (per-folder, default expanded)
 *          - QSet<QString> m_collapsedSections  (per-section, default expanded;
 *                                                 only top-level sections
 *                                                 with `collapsible=true`).
 *        Toggling fires a re-derive. Sections that have ZERO rows after
 *        filter / categorisation hide their header entirely (invariant:
 *        an empty section never emits a header row).
 *
 *        Categorisation rules:
 *          - Pinned section: every conversation with is_pinned = 1
 *            appears here IN ADDITION TO its natural section. Pinning
 *            duplicates rows; it does not move them.
 *          - Projects & Organizations: every project/org/regular folder
 *            tree, with the legacy CONVERSATIONS header inside project
 *            bodies replaced by Group Chats / Direct Chats subsections.
 *          - Standalone Group Chats: convs at root with isGroup=true.
 *          - Direct Agent Chats: convs at root with !isGroup AND
 *            primaryAgentId != "".
 *          - Plain Chats: convs at root with !isGroup AND
 *            primaryAgentId == "".
 *
 * @layer Service (Model)
 * @dependencies ConversationListModel, MembershipService, Qt6::Core
 */


#pragma once

#include <QTimer>

#include <QAbstractListModel>
#include <QDateTime>
#include <QList>
#include <QObject>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVariantMap>

class ConversationListModel;
class MembershipService;

/**
 * @brief QAbstractListModel exposing a single flat list of sidebar rows
 *        derived from the upstream conversation tree + membership state.
 *
 * Owns its own collapse-state caches and re-derives rows in response
 * to upstream signals through a coalescing single-shot timer.
 */
class SidebarFlatModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(QString filterText READ filterText WRITE setFilterText NOTIFY filterTextChanged)

  public:
    /** @brief Item data roles exposed to QML delegates. */
    enum Roles {
        ItemTypeRole = Qt::UserRole + 1,
        ItemIdRole,
        ItemTitleRole,
        ItemSubtitleRole,
        ItemChildCountRole,
        FolderKindRole,
        FolderExpandedRole,
        MemberAliasRole,
        MemberAgentIdRole,
        MemberAgentNameRole,
        MemberIsCoordinatorRole,
        MemberIconNameRole,
        ParentFolderIdRole,
        IsActiveRole,
        IsPinnedRole,            ///< true for conversation rows of a pinned conv.
        SectionIdRole,           ///< stable id for section/subsection headers (collapse key).
        SectionCollapsibleRole,  ///< true for top-level section headers; false for
                                 ///< subsection_header (always-expanded).
        SectionCollapsedRole,    ///< true if user has collapsed this section.
        DepthRole  ///< indent level (0 = top section, 1 = folder, 2 = members/subsections, 3 =
                   ///< convs inside subsection).
    };
    Q_ENUM(Roles)

    /**
     * @brief Constructs the flat model on top of the upstream tree
     *        and membership service.
     * @param tree        Hierarchical conversation tree model. Must outlive
     *                    this object.
     * @param membership  Membership service used to look up project members
     *                    + per-conversation rosters. Must outlive this object.
     * @param parent      Optional Qt parent.
     */
    explicit SidebarFlatModel(ConversationListModel& tree,
                              MembershipService& membership,
                              QObject* parent = nullptr);

    /**
     * @brief Returns the number of flat rows currently exposed.
     * @returns Row count.
     */
    int count() const;

    // QAbstractListModel

    /**
     * @brief QAbstractListModel row count.
     * @param parent  Ignored (this is a flat list model).
     * @returns Row count.
     */
    int rowCount(const QModelIndex& parent = {}) const override;

    /**
     * @brief QAbstractListModel data accessor.
     * @param index  Row index requested.
     * @param role   Custom role from the Roles enum.
     * @returns QVariant holding the role value, or an invalid QVariant
     *          when index / role is out of range.
     */
    QVariant data(const QModelIndex& index, int role) const override;

    /**
     * @brief QAbstractListModel role-name map for QML.
     * @returns Hash mapping Roles enum values to QML-side role names.
     */
    QHash<int, QByteArray> roleNames() const override;

    /**
     * @brief Returns true when the folder with the given id is expanded.
     * @param folderId  Folder UUID.
     * @returns True when expanded; folders default to expanded.
     */
    Q_INVOKABLE bool isFolderExpanded(const QString& folderId) const;

    /**
     * @brief Toggles a folder's expansion state and re-derives rows.
     * @param folderId  Folder UUID.
     *
     * Collapsed folders are kept in m_collapsedFolders.
     */
    Q_INVOKABLE void toggleFolder(const QString& folderId);

    /**
     * @brief Top-level section collapse-state read-back.
     * @param sectionId  One of the stable section ids emitted on
     *                   section_header rows via SectionIdRole.
     * @returns True when the section is currently collapsed. Always
     *          false for empty / non-collapsible ids.
     */
    Q_INVOKABLE bool isSectionCollapsed(const QString& sectionId) const;

    /**
     * @brief Toggle a top-level section's expand/collapse state and
     *        re-derive rows.
     * @param sectionId  Section id. No-op for empty / non-collapsible
     *                   ids. Subsection headers (Group Chats / Direct
     *                   Chats inside project bodies) are always-expanded;
     *                   passing their id is a no-op.
     */
    Q_INVOKABLE void toggleSection(const QString& sectionId);

    /**
     * @brief Direct setter for a section's collapse state. Idempotent.
     * @param sectionId  Section id.
     * @param collapsed  Desired collapse state.
     */
    Q_INVOKABLE void setSectionCollapsed(const QString& sectionId, bool collapsed);

    /**
     * @brief Returns the current case-insensitive search filter.
     * @returns Filter text. Empty means "show everything".
     *
     * When non-empty, the flat list contains only conversation rows
     * whose title matches.
     */
    QString filterText() const;

    /**
     * @brief Sets the case-insensitive search filter and re-derives rows.
     * @param text  Filter substring. Pass empty to clear the filter.
     */
    void setFilterText(const QString& text);

    /**
     * @brief Sets the currently-active conversation id.
     * @param convId  Conversation UUID, or empty if no conversation
     *                is active.
     *
     * Recomputes the IsActiveRole for the previously-active row and the
     * newly-active row and emits dataChanged for both. Called from
     * AppController in response to ChatController::activeConversationChanged.
     */
    void setActiveConversationId(const QString& convId);

    /**
     * @brief Returns the currently-active conversation id.
     * @returns Active conversation UUID, or empty if none.
     */
    QString activeConversationId() const;

  signals:
    /** @brief Emitted whenever the flat row count changes. */
    void countChanged();

    /** @brief Emitted when the search filter text changes. */
    void filterTextChanged();

  private slots:
    /**
     * @brief Schedules a coalesced rebuild when the upstream tree model
     *        emits any structural / content change.
     */
    void onUpstreamChanged();

    /**
     * @brief Schedules a rebuild when a project folder's roster changes.
     * @param folderId  Folder whose membership changed.
     */
    void onProjectMembersChanged(const QString& folderId);

    /**
     * @brief Schedules a rebuild when a conversation's member list changes.
     * @param conversationId  Conversation whose membership changed.
     */
    void onConversationMembersChanged(const QString& conversationId);

  private:
    ConversationListModel& m_tree;
    MembershipService& m_membership;

    /**
     * @brief One flat row emitted by the model. Populated fields depend
     *        on `itemType`; consumers read via the matching role enum.
     */
    struct Row {
        QString itemType;
        QString itemId;
        QString itemTitle;
        QString itemSubtitle;
        int itemChildCount = 0;
        QString folderKind;
        bool folderExpanded = true;
        QString memberAlias;
        QString memberAgentId;
        QString memberAgentName;
        bool memberIsCoordinator = false;
        QString memberIconName;
        QString parentFolderId;
        bool isPinned = false;            ///< conversation row only.
        QString sectionId;                ///< section_header / subsection_header rows.
        bool sectionCollapsible = false;  ///< true on collapsible top-level sections only.
        bool sectionCollapsed = false;    ///< current collapse state of this section.
        int depth = 0;                    ///< indent level: 0 top, deeper inside.
    };

    /** @brief Conversation snapshot used during categorisation. Kept as
     *         a value type so the categoriser can sort buckets by
     *         updated_at DESC and dedup pinned duplicates without
     *         re-querying the tree model. */
    struct ConvCategorised {
        QString id;
        QString title;
        QString subtitle;        ///< updated_at as ISO string.
        QString parentFolderId;  ///< empty == root-level conversation.
        QString primaryAgentId;
        bool isGroup = false;
        bool isPinned = false;
        QDateTime updatedAt;  ///< drives the per-bucket sort.
    };

    QList<Row> m_rows;
    QSet<QString> m_collapsedFolders;
    QSet<QString> m_collapsedSections;  ///< Sticky per-app-run collapse state.
    QString m_filterText;
    QString m_activeConversationId;

    /** Single-shot 0 ms timer that coalesces upstream-signal-driven
     *  rebuilds into one per event-loop pass (see scheduleRebuild()). */
    QTimer m_rebuildTimer;

    static constexpr auto kSectionPinned = "section:pinned";
    static constexpr auto kSectionProjectsOrgs = "section:projects-orgs";
    static constexpr auto kSectionStandaloneGrp = "section:standalone-groups";
    static constexpr auto kSectionDirectAgents = "section:direct-agents";
    static constexpr auto kSectionPlainChats = "section:plain-chats";

    /** @brief Re-derives the flat row list from upstream state. Cancels
     *         any rebuild armed by scheduleRebuild() before running. */
    void rebuild();

    /**
     * @brief Coalesced rebuild trigger. Arms a single-shot 0 ms timer so
     *        a burst of upstream signals (e.g. the dozens fired while a
     *        project room is created) collapses into ONE rebuild on the
     *        next event-loop pass instead of one full reset per signal.
     */
    void scheduleRebuild();

    /**
     * @brief Walks the upstream tree once and gathers every conversation
     *        into a flat ConvCategorised list (folder convs + root convs),
     *        annotating each with parentFolderId / primaryAgentId / isGroup
     *        / isPinned / updatedAt for downstream bucket assignment.
     */
    QList<ConvCategorised> gatherCategorisedConversations() const;

    /**
     * @brief Derived display title for direct-chats
     *        inside project/org folders. Strips the "Chat with @" prefix
     *        and appends the agent's name in parentheses (e.g. "Alice
     *        (Project Manager)") so the per-project Direct Chats
     *        subsection reads cleanly. Returns the original title
     *        unchanged when no membership match is found, so the
     *        function is total: never empty unless the input is empty.
     */
    QString derivedDirectChatTitle(const QString& folderId, const QString& title) const;

    /**
     * @brief Append a section_header row for a top-level collapsible
     *        section. Always uses depth=0 and sectionCollapsible=true.
     */
    void appendSectionHeader(const QString& sectionId, const QString& title, int count);

    /**
     * @brief Append a subsection_header row (project's Group Chats /
     *        Direct Chats). Always-expanded (sectionCollapsible=false).
     */
    void appendSubsectionHeader(const QString& sectionId,
                                const QString& title,
                                int count,
                                const QString& parentFolderId,
                                int depth);

    /**
     * @brief Append one conversation row at the given depth, optionally
     *        with a derived display title overriding the natural title.
     */
    void
    appendConversationRow(const ConvCategorised& c, int depth, const QString& overrideTitle = {});

    /**
     * @brief Walks one folder node in the tree and appends its rows
     *        (folder header → members → subsections → convs).
     */
    void appendFolderRows(const QString& folderId,
                          const QString& folderTitle,
                          const QString& folderKind,
                          const QList<ConvCategorised>& folderConvs,
                          const class QModelIndex& folderIdx);

    /** @brief Filter helper. Returns true iff `c.title` matches the
     *         current m_filterText (case-insensitive substring); empty
     *         filter passes everything. */
    bool matchesFilter(const ConvCategorised& c) const;
};
