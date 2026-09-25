// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file skill-service.h
 * @brief Orchestrates the file-backed skill substrate. Owns three
 *        JSON manifests
 *        (`<AppData>/skills/manifests/{installed-skills,preferred-skills,review-state}.json`)
 *        and the `<AppData>/skills/installed/\<id\>/` install tree.
 *
 *        Single-threaded (Qt main thread) by design: every public
 *        method asserts main-thread residency. JSON manifest writes
 *        are atomic via QSaveFile.
 *
 * @layer Service (top-level, owned by AppController)
 * @dependencies Qt6::Core (QJsonDocument / QSaveFile), SkillParser,
 *               SkillHash, ConversationService (folder-chain walk),
 *               models/skill.h.
 */
#pragma once

#include "../models/skill.h"

#include <QHash>
#include <QObject>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

class ConversationService;

/**
 * @brief File-backed skill library + per-scope preferred-list
 *        manifests. Resolves the effective preferred-skills set for
 *        any conversation / folder lookup at request-build time.
 */
class SkillService : public QObject {
    Q_OBJECT
    /** @brief Number of skills currently installed (approved or not). */
    Q_PROPERTY(int installedCount READ installedCount NOTIFY skillsChanged)
    /** @brief Number of installed skills currently approved-and-not-blocked. */
    Q_PROPERTY(int approvedCount READ approvedCount NOTIFY skillsChanged)

  public:
    /**
     * @brief Construct the skill service.
     * @param convs   Non-owning reference. Used by the resolver to
     *                walk a conversation's folder chain so per-folder
     *                preferred lists apply down-tree.
     * @param parent  Qt parent. AppController passes itself so
     *                ownership matches the unique_ptr member pattern.
     */
    explicit SkillService(ConversationService& convs, QObject* parent = nullptr);
    ~SkillService() override;

    /**
     * @brief Create AppData/skills/{installed,quarantine,manifests}/
     *        and load every manifest into in-memory caches.
     *        Idempotent and safe to call from
     *        AppController::initialize.
     */
    void initialize();

    /**
     * @brief Stop accepting writes; release in-memory state.
     *        Idempotent.
     */
    void shutdown();

    /**
     * @brief Reader for the installedCount Q_PROPERTY.
     * @returns Count of installed skills.
     */
    int installedCount() const { return m_installed.size(); }

    /**
     * @brief Reader for the approvedCount Q_PROPERTY.
     * @returns Count of installed skills that are approved AND not
     *          blocked.
     */
    int approvedCount() const;

    // ----- Library queries -----

    /**
     * @brief Return every installed skill as a QML-friendly variant
     *        list.
     * @returns QVariantList of skill projections (id, name, summary,
     *          version, approved, blocked, etc.).
     */
    Q_INVOKABLE QVariantList installedSkills() const;

    /**
     * @brief Return only the approved-and-not-blocked skills.
     * @returns QVariantList of skill projections.
     */
    Q_INVOKABLE QVariantList approvedSkills() const;

    /**
     * @brief Return the full detail map for a single skill.
     * @param skillId Skill id.
     * @returns QVariantMap with the full skill row + review state;
     *          empty when the id is not installed.
     */
    Q_INVOKABLE QVariantMap skillDetails(const QString& skillId) const;

    /**
     * @brief Read the contents of a file inside an installed skill.
     * @param skillId      Skill id to read from.
     * @param relativePath Path within the skill's install tree.
     * @returns File contents as UTF-8 text; empty on read failure or
     *          path-escape rejection.
     */
    Q_INVOKABLE QString readSkillFile(const QString& skillId, const QString& relativePath) const;

    /**
     * @brief C++ accessor for the in-memory Skill POD.
     * @param skillId Skill id.
     * @returns Skill POD; `isValid()==false` when the id is not
     *          installed.
     */
    Skill skillById(const QString& skillId) const;

    /**
     * @brief Absolute on-disk path of an installed skill.
     * @param skillId Skill id.
     * @returns The install directory path, or empty string if the id
     *          is not installed.
     */
    QString installPathFor(const QString& skillId) const;

    // ----- Lifecycle -----

    /**
     * @brief Validate, hash, and install a skill folder picked by
     *        the user (manual import path).
     *
     *        On success: skill is placed in
     *        `skills/installed/\<id\>/`, its row is added to
     *        installed-skills.json, and review-state.json gets a
     *        fresh `unreviewed` row. Emits `skillsChanged`.
     *
     *        On parser failure: the source folder is left untouched;
     *        no manifest changes; returns the parser's error
     *        message.
     *
     *        Re-importing the same id with different content:
     *        existing install dir is replaced (atomically via
     *        remove+rename), review state resets to `unreviewed`
     *        (hash changed).
     *
     * @param folderPath Absolute path the user picked (their
     *                   original folder is NOT moved or modified).
     * @param idHint     Optional id override; when empty the
     *                   manifest's declared id is used.
     * @returns Empty string on success; human-readable error
     *          otherwise.
     */
    Q_INVOKABLE QString importSkillFolder(const QString& folderPath, const QString& idHint = {});

    /**
     * @brief Install a skill from a downloaded ZIP archive.
     *
     *        Extracts via SkillArchive::extract (KZip + size caps),
     *        parses the staging dir via SkillParser, then runs the
     *        same installation pipeline as importSkillFolder. On any
     *        rejection (extraction or parse), the staging dir is
     *        moved into skills/quarantine/.
     *
     *        The downloaded zip itself is NOT deleted by this method;
     *        the caller (typically a slot wired to
     *        ClawHubClient::downloadCompleted) decides retention.
     *
     * @param zipPath    Absolute path to the .zip archive on disk.
     * @param sourceUrl  Optional ClawHub URL to record on the skill
     *                   row. When empty, source defaults to
     *                   "manual".
     * @param expectedId Optional id override; when non-empty rejects
     *                   the install if the parsed id does not match.
     * @returns Empty string on success; error message otherwise.
     */
    Q_INVOKABLE QString installFromZip(const QString& zipPath,
                                       const QString& sourceUrl = {},
                                       const QString& expectedId = {});

    /**
     * @brief Approve a skill so it becomes eligible for resolver
     *        inclusion.
     * @param skillId Skill id to approve.
     * @returns true on success; false on unknown skill id.
     */
    Q_INVOKABLE bool approveSkill(const QString& skillId);

    /**
     * @brief Block a skill so it is excluded from resolver results
     *        regardless of preferred-list membership.
     * @param skillId Skill id to block.
     * @returns true on success; false on unknown skill id.
     */
    Q_INVOKABLE bool blockSkill(const QString& skillId);

    /**
     * @brief Hard-delete a skill. Removes installed-skills row,
     *        review-state row, AND every reference in
     *        preferred-skills.json scope rows, AND the on-disk
     *        folder. There is no soft delete.
     * @param skillId Skill id to remove.
     * @returns true on success; false on unknown skill id or IO
     *          failure.
     */
    Q_INVOKABLE bool removeSkill(const QString& skillId);

    // ----- Per-scope preferred lists -----

    /**
     * @brief Return the preferred-skill id list for a scope.
     * @param scopeType `"conversation"` or `"folder"`.
     * @param scopeId   Conversation UUID or folder UUID.
     * @returns Ordered list of preferred skill ids; empty when the
     *          scope has no curated list.
     */
    Q_INVOKABLE QStringList preferredSkillsFor(const QString& scopeType,
                                               const QString& scopeId) const;

    /**
     * @brief Replace the preferred-skill id list for a scope.
     * @param scopeType `"conversation"` or `"folder"`.
     * @param scopeId   Conversation UUID or folder UUID.
     * @param skillIds  New ordered list of skill ids.
     * @returns true on success; false on IO failure.
     */
    Q_INVOKABLE bool setPreferredSkills(const QString& scopeType,
                                        const QString& scopeId,
                                        const QStringList& skillIds);

    /**
     * @brief Reader: whether the scope's preferred list is
     *        expose-only (the resolver sees the curated list as the
     *        complete tool surface) vs additive.
     * @param scopeType `"conversation"` or `"folder"`.
     * @param scopeId   Conversation UUID or folder UUID.
     * @returns true iff expose-only is set on the scope.
     */
    Q_INVOKABLE bool exposeOnlyPreferred(const QString& scopeType, const QString& scopeId) const;

    /**
     * @brief Setter for the expose-only flag on a scope.
     * @param scopeType `"conversation"` or `"folder"`.
     * @param scopeId   Conversation UUID or folder UUID.
     * @param value     New value for the flag.
     * @returns true on success; false on IO failure.
     */
    Q_INVOKABLE bool
    setExposeOnlyPreferred(const QString& scopeType, const QString& scopeId, bool value);

    /**
     * @brief Reader: whether a conversation overrides its parent
     *        folder's preferred-skill list (only applies when the
     *        conversation lives inside a folder).
     * @param conversationId Conversation UUID.
     * @returns true iff the conversation has an active override
     *          flag.
     */
    Q_INVOKABLE bool overrideParentFolder(const QString& conversationId) const;

    /**
     * @brief Setter for the parent-override flag on a conversation.
     * @param conversationId Conversation UUID.
     * @param value          New value for the flag.
     * @returns true on success; false on IO failure.
     */
    Q_INVOKABLE bool setOverrideParentFolder(const QString& conversationId, bool value);

    // ----- Resolver (consumed by RequestBuilder + heartbeat) -----

    /**
     * @brief Result of preferred-list resolution for a request
     *        build.
     *
     *        Only IDs that are installed AND approved AND not
     *        blocked flow through. The list is in user-curated
     *        order.
     */
    struct ResolvedScope {
        QStringList preferredSkillIds;  ///< Approved preferred skills, in user order.
        bool exposeOnly = false;        ///< True when only these skills are offered.
    };

    /**
     * @brief Resolve the effective preferred-list for a
     *        conversation:
     *
     *          IF conv override flag ON AND conv preferred non-empty
     *              → conv preferred
     *          ELSE IF folder (project) preferred non-empty
     *              → folder preferred
     *          ELSE IF conv preferred non-empty (standalone)
     *              → conv preferred
     *          ELSE → empty
     *
     *        Filtered to approved-only. Non-const because
     *        ConversationService::getConversation is non-const.
     * @param conversationId Conversation UUID.
     * @returns ResolvedScope carrying the ordered preferred-id list
     *          and the expose-only flag.
     */
    ResolvedScope resolveForConversation(const QString& conversationId);

    /**
     * @brief Resolve the preferred-list for a folder directly (used
     *        by folder-scope heartbeat configs). No override walk.
     * @param folderId Folder UUID.
     * @returns ResolvedScope carrying the ordered preferred-id list
     *          and the expose-only flag.
     */
    ResolvedScope resolveForFolder(const QString& folderId) const;

    // ----- Test seams -----

    /**
     * @brief Override the AppData root used by the service (for
     *        unit tests).
     * @param absRoot Absolute path to use in place of
     *                QStandardPaths::AppDataLocation.
     */
    void setAppDataRootForTesting(const QString& absRoot);

  signals:
    /** @brief Notifier: installed / approved / blocked / removed
     *         any skill (drives the Q_PROPERTY count refresh). */
    void skillsChanged();

    /**
     * @brief Emitted when a scope's preferred-skill list mutates.
     * @param scopeType `"conversation"` or `"folder"`.
     * @param scopeId   Conversation / folder UUID.
     */
    void preferredSkillsChanged(QString scopeType, QString scopeId);

    /**
     * @brief User-visible error channel (manifest write failure,
     *        extraction failure, etc.).
     * @param message Human-readable error description.
     */
    void errorOccurred(QString message);

  private:
    // Manifest paths (computed on initialize from m_appDataRoot).
    QString m_appDataRoot;
    QString m_skillsRoot;     ///< <AppData>/skills
    QString m_installedDir;   ///< <AppData>/skills/installed
    QString m_quarantineDir;  ///< <AppData>/skills/quarantine
    QString m_manifestsDir;   ///< <AppData>/skills/manifests
    QString m_installedManifestPath;
    QString m_preferredManifestPath;
    QString m_reviewManifestPath;

    // In-memory caches loaded from disk on initialize, kept in sync on
    // every mutation.
    QHash<QString, Skill> m_installed;             ///< by id
    QHash<QString, SkillReviewDecision> m_review;  ///< by id
    QList<PreferredSkillsScope> m_preferred;

    bool m_initialized = false;

    ConversationService& m_convs;

    // ----- Internal helpers -----
    void ensureDirsExist();
    void loadInstalledManifest();
    void loadPreferredManifest();
    void loadReviewManifest();
    bool writeInstalledManifest();
    bool writePreferredManifest();
    bool writeReviewManifest();

    bool
    writeJsonAtomic(const QString& absPath, const QJsonObject& obj, QString* outError = nullptr);
    bool removeFolderRecursive(const QString& absPath);
    bool copyFolderRecursive(const QString& src, const QString& dst, QString* outError);

    PreferredSkillsScope* findPreferredScope(const QString& type, const QString& id);
    const PreferredSkillsScope* findPreferredScope(const QString& type, const QString& id) const;
    PreferredSkillsScope& getOrCreatePreferredScope(const QString& type, const QString& id);

    QVariantMap skillToVariantMap(const Skill& s) const;
};
