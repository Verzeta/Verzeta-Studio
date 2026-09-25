// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file project-template-service.h
 * @brief Curated library of starter project templates and the
 *        transactional "create project from template" pipeline.
 * @layer Service
 * @dependencies Qt6::Core, Qt6::Sql; ConversationService,
 *               MembershipService, AgentRegistry (non-owning refs);
 *               DbManager (for transaction bracket).
 *
 * Catalog source: bundled JSON files at `:/project-templates/<file>.json`,
 * one file per template, registered on the verzeta-studio-backend static
 * library's QRC via qt_add_resources in `backend/CMakeLists.txt`. It is the
 * backend library, NOT the QML module, so test binaries that link the
 * backend can read the catalog too.
 *
 * Output: a fully-populated project folder (rows in `folders` with
 * folder_type='project' + members in `project_members`) created
 * inside a single DB transaction. Either every write commits or
 * every write rolls back, leaving no half-created projects.
 *
 * QML exposure: registered as the `ProjectTemplates` singleton in
 * `AppController::registerTypes()`. The Project Rooms overlay reads
 * `templates()` / `templateById(id)` and invokes
 * `createProjectFromTemplate(id, customisations)`.
 *
 * Threading: strictly main-thread.
 */

#pragma once

#include <QHash>
#include <QObject>
#include <QSet>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

class AgentRegistry;
class ConversationService;
class DbManager;
class MembershipService;

/**
 * @brief Catalog + create-from-template pipeline for project-folder
 *        templates. Bundles built-in JSON files at `:/project-templates`
 *        and user-saved JSON files under `<AppData>/user-templates`,
 *        and creates fully-populated project folders inside a single
 *        SQL transaction.
 */
class ProjectTemplateService : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Construct the service, loading the catalog and pinned
     *        set at construction time.
     * @param convSvc          Non-owning ConversationService reference
     *                         used to write the folder row.
     * @param membershipSvc    Non-owning MembershipService reference
     *                         used to write project_members rows.
     * @param agentRegistry    Non-owning AgentRegistry reference used
     *                         to resolve template member agent names.
     * @param db               Non-owning DbManager reference used for
     *                         the transaction bracket.
     * @param userTemplateDir  Directory the user-saved template JSON
     *                         files live in. Empty (the production
     *                         default) resolves to
     *                         `\<AppDataLocation\>/user-templates`.
     *                         Tests pass an explicit temp dir.
     * @param parent           Qt parent.
     */
    ProjectTemplateService(ConversationService& convSvc,
                           MembershipService& membershipSvc,
                           AgentRegistry& agentRegistry,
                           DbManager& db,
                           const QString& userTemplateDir = QString(),
                           QObject* parent = nullptr);
    ~ProjectTemplateService() override;

    /**
     * @brief The full template catalog. Each entry is a QVariantMap
     *        mirroring the JSON schema: id / name / tagLabel /
     *        category / geometryKind / baseHue / scenario / goal /
     *        description / coordinator / members / seedDocuments.
     * @returns Full catalog as a QVariantList of template maps.
     */
    Q_INVOKABLE QVariantList templates() const;

    /**
     * @brief Look up a template by id.
     * @param id Template id from `templates()`.
     * @returns Template map, or empty map if the id is unknown.
     */
    Q_INVOKABLE QVariantMap templateById(const QString& id) const;

    /**
     * @brief The bounded catalog shown on the Project Rooms landing:
     *        every built-in template plus the user templates the user
     *        has pinned. Keeps the landing panel from growing
     *        unbounded as the user saves templates; the full set
     *        lives in the Template Library served by `templates()`.
     * @returns Landing-pinned template list.
     */
    Q_INVOKABLE QVariantList landingTemplates() const;

    /**
     * @brief Returns the template's roster (coordinator first, then
     *        members in order) resolved to the shape
     *        `MembershipEditor.initialMembers` expects: a
     *        QVariantList of `{agentId, alias, isCoordinator,
     *        agentName, iconName}`.
     *
     *        Templates reference agents by NAME because UUIDs are
     *        per-install, so this resolves each name to a real
     *        agents.id via AgentRegistry so the Quick Start form can
     *        seed MembershipEditor directly. Members whose agent
     *        name does not resolve are skipped.
     * @param templateId Template id whose roster to resolve.
     * @returns Roster list; empty when templateId is unknown.
     */
    Q_INVOKABLE QVariantList templateRoster(const QString& templateId) const;

    /**
     * @brief Creates a new project folder from a template inside a
     *        single SQL transaction.
     *
     *        On success: folder row written, every member added with
     *        the coordinator flagged correctly, signal emitted,
     *        folderId returned.
     *
     *        On any failure (unknown template, unknown agent name,
     *        DB error): transaction rolled back, returns empty
     *        string. No half-created state remains.
     *
     * @param templateId    Id from `templates()`.
     * @param customisations Optional overrides. Recognised keys:
     *                       `name`, `goal`, `description`, `scenario`,
     *                       `members`. A present `members` key REPLACES
     *                       the template's default roster entirely.
     *                       Each member row is a QVariantMap with an
     *                       `alias` + `isCoordinator`, and identifies
     *                       its agent EITHER by `agentId` (a real
     *                       agents.id, which is what MembershipEditor.member
     *                       List() produces) OR by `agentTemplate` (a
     *                       built-in template name, which is what the bundled
     *                       JSON templates carry). `agentId` wins when
     *                       both are present. Optional per-member
     *                       `modelProvider` / `modelName` /
     *                       `allowedTools` overrides ride through to
     *                       the project_members row. Absent keys fall
     *                       back to the template's built-in defaults.
     * @return New folder UUID on success, empty string on failure.
     */
    Q_INVOKABLE QString createProjectFromTemplate(const QString& templateId,
                                                  const QVariantMap& customisations = {});

    /**
     * @brief Pin or unpin a USER template so it also surfaces on the
     *        Project Rooms landing. Built-in templates are always on
     *        the landing and cannot be pinned / unpinned; passing a
     *        built-in (or unknown) id returns false. Persists
     *        `pinned.json` and emits `catalogChanged()`.
     * @param templateId Template id to pin or unpin.
     * @param pinned     true to pin; false to unpin.
     * @returns true on success.
     */
    Q_INVOKABLE bool setTemplatePinned(const QString& templateId, bool pinned);

    /**
     * @brief Write a new user template JSON under the user-template
     *        directory and add it to the live catalog.
     * @param sourceTemplateId  Optional. When a known id, its
     *        category / tagLabel / geometryKind / baseHue seed the new
     *        template; empty starts from neutral defaults.
     * @param edits  name / scenario / goal / description / members
     *        (the `MembershipEditor.memberList()` shape: each member a
     *        map with `agentId` / `alias` / `isCoordinator` and optional
     *        per-member overrides).
     * @return The new template's id, or empty string on failure.
     */
    Q_INVOKABLE QString saveAsNewTemplate(const QString& sourceTemplateId,
                                          const QVariantMap& edits);

    /**
     * @brief Delete a USER template: its JSON file, its catalog row
     *        and any pin. No-op returning false for a built-in or
     *        unknown id. Emits `catalogChanged()` on success.
     * @param templateId User template id to delete.
     * @returns true on success.
     */
    Q_INVOKABLE bool deleteUserTemplate(const QString& templateId);

  signals:
    /**
     * @brief Emitted after a successful create. The Project Rooms
     *        overlay subscribes and triggers the kickoff sheet with
     *        the new folder so the user can immediately spin up the
     *        first 1:1 chat and / or group chat.
     * @param folderId    UUID of the newly-created project folder.
     * @param folderName  Display name of the new folder.
     * @param memberCount Number of members added to the folder.
     */
    void
    projectCreatedFromTemplate(const QString& folderId, const QString& folderName, int memberCount);

    /**
     * @brief Emitted whenever the catalog changes: a user template
     *        saved or deleted, or a pin toggled. The Project Rooms
     *        overlay + Template Library re-read `templates()` /
     *        `landingTemplates()` on this.
     */
    void catalogChanged();

  private:
    /**
     * @brief Loaded template row. Mirrors the JSON file but kept as a
     *        QVariantMap so it can be re-emitted via Q_INVOKABLE
     *        directly without a second copy.
     */
    QList<QVariantMap> m_templates;
    /** id → index into `m_templates`. O(1) lookup. */
    QHash<QString, int> m_indexById;

    // Non-owning. AppController owns all four; lifetime guaranteed.
    ConversationService& m_convSvc;
    MembershipService& m_membershipSvc;
    AgentRegistry& m_agentRegistry;
    DbManager& m_db;

    /** Directory the user-saved template JSON files live in. Resolved
     *  once at construction: `\<AppDataLocation\>/user-templates` in
     *  production, an explicit temp dir under test. */
    QString m_userTemplateDir;

    /** Ids of user templates pinned to the landing. Mirrors
     *  `<m_userTemplateDir>/pinned.json`. Built-in ids never appear. */
    QSet<QString> m_pinnedIds;

    /**
     * @brief Read every bundled JSON file under `:/project-templates/`
     *        at construction. Malformed files are skipped and logged;
     *        the catalog still loads what it can. Built-in rows are
     *        stamped `isUserSaved = false`.
     */
    void loadCatalog();

    /** Walk `m_userTemplateDir` for `*.json` (excluding `pinned.json`)
     *  and append each valid one to the catalog, stamped
     *  `isUserSaved = true`. Ids already present are skipped. */
    void loadUserTemplates();

    /** Read `<m_userTemplateDir>/pinned.json` into `m_pinnedIds`. */
    void loadPinnedSet();

    /** Persist `m_pinnedIds` to `<m_userTemplateDir>/pinned.json`
     *  atomically. Returns false on write failure. */
    bool savePinnedSet();
};
