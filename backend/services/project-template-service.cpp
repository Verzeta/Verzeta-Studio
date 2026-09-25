// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file project-template-service.cpp
 * @brief ProjectTemplateService implementation: catalog load from
 *        QRC + transactional "create from template" pipeline.
 * @layer Service
 * @dependencies ConversationService, MembershipService, AgentRegistry,
 *               Qt6::Core, Qt6::Sql.
 */

#include "project-template-service.h"

#include "../models/agent.h"
#include "../models/db-manager.h"
#include "../services/agent-registry.h"
#include "../services/conversation-service.h"
#include "../services/membership-service.h"
#include "../utils/logger.h"
#include "../utils/thread-discipline.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QStandardPaths>
#include <QUuid>

namespace {

// Backend-library QRC prefix. The eight template JSON files are added
// to the `verzeta-studio-backend` static library via qt_add_resources
// in `backend/CMakeLists.txt` so they're accessible to BOTH the main
// executable AND any test binary that links the backend library.
// The QML-module resource tree is the wrong home for these because
// test binaries don't link the QML module.
constexpr auto kQrcPrefix = ":/project-templates/";

// Filenames that the build registers — listed explicitly because
// QDir over a QRC root in Qt 6 doesn't always enumerate cleanly.
const QStringList& bundledFilenames() {
    static const QStringList kFiles = {
        QStringLiteral("01-research-brief.json"),
        QStringLiteral("02-customer-interview-synthesis.json"),
        QStringLiteral("03-code-review-war-room.json"),
        QStringLiteral("04-technical-spec.json"),
        QStringLiteral("05-marketing-copy-studio.json"),
        QStringLiteral("06-product-launch-plan.json"),
        QStringLiteral("07-sales-pitch-workshop.json"),
        QStringLiteral("08-quarterly-business-review.json"),
    };
    return kFiles;
}

QVariantMap parseTemplateBytes(const QByteArray& bytes, const QString& filenameForLog) {
    QJsonParseError err{};
    const auto doc = QJsonDocument::fromJson(bytes, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        qCWarning(verzetaUi) << "ProjectTemplateService: skipping malformed template"
                             << filenameForLog << "—" << err.errorString();
        return {};
    }
    return doc.object().toVariantMap();
}

}  // namespace

// ---------------------------------------------------------------------------

ProjectTemplateService::ProjectTemplateService(ConversationService& convSvc,
                                               MembershipService& membershipSvc,
                                               AgentRegistry& agentRegistry,
                                               DbManager& db,
                                               const QString& userTemplateDir,
                                               QObject* parent)
    : QObject(parent)
    , m_convSvc(convSvc)
    , m_membershipSvc(membershipSvc)
    , m_agentRegistry(agentRegistry)
    , m_db(db)
    , m_userTemplateDir(userTemplateDir.isEmpty()
                            ? QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                                  QStringLiteral("/user-templates")
                            : userTemplateDir) {
    loadCatalog();        // built-in templates from the backend QRC
    loadPinnedSet();      // <user-template-dir>/pinned.json
    loadUserTemplates();  // <user-template-dir>/*.json
    qCInfo(verzetaUi) << "ProjectTemplateService: catalog has" << m_templates.size() << "templates;"
                      << m_pinnedIds.size() << "pinned to landing";
}

ProjectTemplateService::~ProjectTemplateService() = default;

void ProjectTemplateService::loadCatalog() {
    VERZETA_ASSERT_MAIN_THREAD();
    m_templates.clear();
    m_indexById.clear();

    for (const QString& filename : bundledFilenames()) {
        const QString path = QString::fromLatin1(kQrcPrefix) + filename;
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            qCWarning(verzetaUi) << "ProjectTemplateService: failed to open" << path << "—"
                                 << f.errorString();
            continue;
        }
        const QByteArray bytes = f.readAll();
        f.close();

        QVariantMap row = parseTemplateBytes(bytes, filename);
        if (row.isEmpty())
            continue;

        const QString id = row.value(QStringLiteral("id")).toString();
        if (id.isEmpty()) {
            qCWarning(verzetaUi) << "ProjectTemplateService: template" << filename
                                 << "missing required field `id` — skipping";
            continue;
        }
        if (m_indexById.contains(id)) {
            qCWarning(verzetaUi) << "ProjectTemplateService: duplicate template id" << id << "in"
                                 << filename << "— skipping";
            continue;
        }
        row.insert(QStringLiteral("isUserSaved"), false);
        m_indexById.insert(id, m_templates.size());
        m_templates.append(row);
    }
}

void ProjectTemplateService::loadUserTemplates() {
    VERZETA_ASSERT_MAIN_THREAD();
    QDir dir(m_userTemplateDir);
    if (!dir.exists()) {
        return;  // first run — no user templates yet
    }
    const QStringList files =
        dir.entryList(QStringList{QStringLiteral("*.json")}, QDir::Files, QDir::Name);
    for (const QString& filename : files) {
        if (filename == QStringLiteral("pinned.json"))
            continue;

        const QString path = dir.filePath(filename);
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
            qCWarning(verzetaUi) << "ProjectTemplateService: failed to open user template" << path
                                 << "—" << f.errorString();
            continue;
        }
        const QByteArray bytes = f.readAll();
        f.close();

        QVariantMap row = parseTemplateBytes(bytes, filename);
        if (row.isEmpty())
            continue;

        const QString id = row.value(QStringLiteral("id")).toString();
        if (id.isEmpty()) {
            qCWarning(verzetaUi) << "ProjectTemplateService: user template" << filename
                                 << "missing required field `id` — skipping";
            continue;
        }
        if (m_indexById.contains(id)) {
            qCWarning(verzetaUi) << "ProjectTemplateService: user template id" << id
                                 << "collides with an existing template — skipping";
            continue;
        }
        row.insert(QStringLiteral("isUserSaved"), true);
        m_indexById.insert(id, m_templates.size());
        m_templates.append(row);
    }
}

void ProjectTemplateService::loadPinnedSet() {
    VERZETA_ASSERT_MAIN_THREAD();
    m_pinnedIds.clear();
    QFile f(m_userTemplateDir + QStringLiteral("/pinned.json"));
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return;  // no pin manifest yet
    }
    const QByteArray bytes = f.readAll();
    f.close();

    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
    if (err.error != QJsonParseError::NoError || !doc.isArray()) {
        qCWarning(verzetaUi) << "ProjectTemplateService: malformed pinned.json —"
                             << err.errorString();
        return;
    }
    for (const QJsonValue& v : doc.array()) {
        const QString id = v.toString();
        if (!id.isEmpty())
            m_pinnedIds.insert(id);
    }
}

bool ProjectTemplateService::savePinnedSet() {
    VERZETA_ASSERT_MAIN_THREAD();
    QDir().mkpath(m_userTemplateDir);

    QJsonArray arr;
    for (const QString& id : m_pinnedIds)
        arr.append(id);
    const QByteArray bytes = QJsonDocument(arr).toJson(QJsonDocument::Indented);

    QSaveFile f(m_userTemplateDir + QStringLiteral("/pinned.json"));
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qCWarning(verzetaUi) << "ProjectTemplateService: cannot write pinned.json —"
                             << f.errorString();
        return false;
    }
    f.write(bytes);
    if (!f.commit()) {
        qCWarning(verzetaUi) << "ProjectTemplateService: pinned.json commit failed —"
                             << f.errorString();
        return false;
    }
    return true;
}

QVariantList ProjectTemplateService::templates() const {
    VERZETA_ASSERT_MAIN_THREAD();
    QVariantList out;
    out.reserve(m_templates.size());
    for (const QVariantMap& t : m_templates) {
        QVariantMap row = t;
        row.insert(QStringLiteral("isPinned"),
                   m_pinnedIds.contains(row.value(QStringLiteral("id")).toString()));
        out.append(row);
    }
    return out;
}

QVariantMap ProjectTemplateService::templateById(const QString& id) const {
    VERZETA_ASSERT_MAIN_THREAD();
    const int idx = m_indexById.value(id, -1);
    if (idx < 0 || idx >= m_templates.size()) {
        return {};
    }
    QVariantMap row = m_templates.at(idx);
    row.insert(QStringLiteral("isPinned"), m_pinnedIds.contains(id));
    return row;
}

QVariantList ProjectTemplateService::landingTemplates() const {
    VERZETA_ASSERT_MAIN_THREAD();
    QVariantList out;
    for (const QVariantMap& t : m_templates) {
        const QString id = t.value(QStringLiteral("id")).toString();
        const bool userSaved = t.value(QStringLiteral("isUserSaved")).toBool();
        // Built-ins are always on the landing; a user template only
        // when the user has pinned it. Everything else lives in the
        // Template Library (served by templates()).
        if (userSaved && !m_pinnedIds.contains(id))
            continue;
        QVariantMap row = t;
        row.insert(QStringLiteral("isPinned"), m_pinnedIds.contains(id));
        out.append(row);
    }
    return out;
}

QVariantList ProjectTemplateService::templateRoster(const QString& templateId) const {
    VERZETA_ASSERT_MAIN_THREAD();
    QVariantList out;

    const int idx = m_indexById.value(templateId, -1);
    if (idx < 0) {
        return out;
    }
    const QVariantMap tpl = m_templates.at(idx);

    // Resolve one member row → a MembershipEditor.initialMembers row.
    // A member is identified EITHER by `agentId` (user templates —
    // saved straight from the editor) OR by `agentTemplate` name (the
    // bundled JSON templates). Skips silently when it does not resolve
    // — the roster is a best-effort seed, not a transactional write.
    const auto appendMember = [&](const QVariantMap& memberRow, bool isCoordinator) {
        const QString alias = memberRow.value(QStringLiteral("alias")).toString();
        if (alias.isEmpty())
            return;

        Agent agent;
        const QString agentId = memberRow.value(QStringLiteral("agentId")).toString();
        if (!agentId.isEmpty()) {
            agent = m_agentRegistry.getAgent(agentId);
        } else {
            agent = m_agentRegistry.getAgentByName(
                memberRow.value(QStringLiteral("agentTemplate")).toString());
        }
        if (!agent.isValid())
            return;

        out.append(QVariantMap{
            {QStringLiteral("agentId"), agent.id},
            {QStringLiteral("alias"), alias},
            {QStringLiteral("isCoordinator"), isCoordinator},
            {QStringLiteral("agentName"), agent.name},
            {QStringLiteral("iconName"), agent.iconName},
            // Per-member overrides ride through so the editor shows a
            // user template's saved provider / model / tools. Absent
            // on built-in members → empty.
            {QStringLiteral("modelProvider"),
             memberRow.value(QStringLiteral("modelProvider")).toString()},
            {QStringLiteral("modelName"), memberRow.value(QStringLiteral("modelName")).toString()},
            {QStringLiteral("allowedTools"),
             memberRow.value(QStringLiteral("allowedTools")).toStringList()},
        });
    };

    // Built-in style: a top-level `coordinator` names the coordinator
    // template, and its alias is that same name (matching the default
    // roster createProjectFromTemplate builds). User templates carry
    // no top-level coordinator — every member, coordinator included,
    // sits in members[] with its own isCoordinator flag.
    const QString coord = tpl.value(QStringLiteral("coordinator")).toString();
    if (!coord.isEmpty()) {
        appendMember(
            QVariantMap{
                {QStringLiteral("agentTemplate"), coord},
                {QStringLiteral("alias"), coord},
            },
            /*isCoordinator=*/true);
    }

    const QVariantList tplMembers = tpl.value(QStringLiteral("members")).toList();
    for (const QVariant& m : tplMembers) {
        const QVariantMap mm = m.toMap();
        appendMember(mm, mm.value(QStringLiteral("isCoordinator")).toBool());
    }
    return out;
}

QString ProjectTemplateService::createProjectFromTemplate(const QString& templateId,
                                                          const QVariantMap& customisations) {
    VERZETA_ASSERT_MAIN_THREAD();

    // An empty templateId is the "Blank project" path — the folder is
    // built purely from `customisations` (name / goal / description /
    // members the user filled in on the Quick Start form), with no
    // template lookup at all.
    const bool blank = templateId.isEmpty();
    QVariantMap tpl;
    if (!blank) {
        const int idx = m_indexById.value(templateId, -1);
        if (idx < 0) {
            qCWarning(verzetaUi) << "ProjectTemplateService::createProjectFromTemplate:"
                                 << "unknown template id" << templateId;
            return {};
        }
        tpl = m_templates.at(idx);
    }

    // Resolve fields with customisation overrides taking precedence.
    auto pick = [&](const QString& key) {
        const QVariant cust = customisations.value(key);
        if (cust.isValid() && !cust.toString().isEmpty()) {
            return cust.toString();
        }
        return tpl.value(key).toString();
    };
    const QString name = pick(QStringLiteral("name"));
    const QString goal = pick(QStringLiteral("goal"));
    const QString description = pick(QStringLiteral("description"));

    // Compose effective member list. Customisations.members overrides
    // the template's defaults entirely (caller is responsible for
    // including every member they want, including the coordinator).
    QVariantList effectiveMembers;
    if (customisations.contains(QStringLiteral("members"))) {
        effectiveMembers = customisations.value(QStringLiteral("members")).toList();
    } else {
        // Build from template defaults: coordinator first, then
        // members[] in order.
        const QString coord = tpl.value(QStringLiteral("coordinator")).toString();
        if (!coord.isEmpty()) {
            QVariantMap coordRow;
            coordRow.insert(QStringLiteral("alias"), coord);
            coordRow.insert(QStringLiteral("agentTemplate"), coord);
            coordRow.insert(QStringLiteral("isCoordinator"), true);
            effectiveMembers.append(coordRow);
        }
        const QVariantList tplMembers = tpl.value(QStringLiteral("members")).toList();
        for (const QVariant& m : tplMembers) {
            QVariantMap mm = m.toMap();
            mm.insert(QStringLiteral("isCoordinator"), false);
            effectiveMembers.append(mm);
        }
    }

    if (name.isEmpty()) {
        qCWarning(verzetaUi) << "ProjectTemplateService::createProjectFromTemplate:"
                             << "empty effective name for" << templateId;
        return {};
    }

    // -- Transaction bracket -----------------------------------------
    QSqlDatabase database = m_db.db();
    if (!database.transaction()) {
        qCWarning(verzetaUi) << "ProjectTemplateService: failed to begin transaction —"
                             << database.lastError().text();
        return {};
    }

    auto rollbackAndFail = [&database](const QString& reason) -> QString {
        qCWarning(verzetaUi) << "ProjectTemplateService: rolling back create —" << reason;
        database.rollback();
        return {};
    };

    // 1. Create the folder row.
    const QString folderId = m_convSvc.createFolder(name, /*parentId=*/{});
    if (folderId.isEmpty()) {
        return rollbackAndFail(QStringLiteral("createFolder returned empty id"));
    }

    // 2. Promote to project type + set goal / description.
    if (!m_convSvc.updateFolderMetadata(folderId,
                                        QStringLiteral("project"),
                                        goal,
                                        description,
                                        /*agentIds=*/{})) {
        return rollbackAndFail(QStringLiteral("updateFolderMetadata failed"));
    }

    // 3. Resolve every member and add it to the project.
    //
    //    A member row is identified EITHER by `agentId` (a real
    //    agents.id UUID — what MembershipEditor.memberList() produces
    //    when the roster has been edited in the Quick Start form) OR
    //    by `agentTemplate` (a built-in template NAME — what the
    //    bundled JSON templates carry, since agent UUIDs are
    //    per-install and a template file cannot hard-code them).
    //    `agentId` wins when both are present. Per-member provider /
    //    model / tool-whitelist overrides (present only when the
    //    roster was edited) ride straight through to addProjectMember.
    int coordinatorCount = 0;
    for (const QVariant& memberVar : effectiveMembers) {
        const QVariantMap memberMap = memberVar.toMap();
        const QString alias = memberMap.value(QStringLiteral("alias")).toString();
        const bool isCoordinator = memberMap.value(QStringLiteral("isCoordinator")).toBool();

        if (alias.isEmpty()) {
            return rollbackAndFail(QStringLiteral("member row missing alias"));
        }

        // Resolve the agent — by id first, then by template name.
        QString resolvedAgentId = memberMap.value(QStringLiteral("agentId")).toString();
        if (!resolvedAgentId.isEmpty()) {
            if (!m_agentRegistry.getAgent(resolvedAgentId).isValid()) {
                return rollbackAndFail(QStringLiteral("unknown agentId: ") + resolvedAgentId);
            }
        } else {
            const QString agentTemplateName =
                memberMap.value(QStringLiteral("agentTemplate")).toString();
            if (agentTemplateName.isEmpty()) {
                return rollbackAndFail(
                    QStringLiteral("member row missing both agentId and agentTemplate"));
            }
            const Agent agent = m_agentRegistry.getAgentByName(agentTemplateName);
            if (!agent.isValid()) {
                return rollbackAndFail(QStringLiteral("agent template not found: ") +
                                       agentTemplateName);
            }
            resolvedAgentId = agent.id;
        }

        if (isCoordinator)
            ++coordinatorCount;
        if (coordinatorCount > 1) {
            return rollbackAndFail(QStringLiteral("more than one coordinator in member list"));
        }

        // Per-member overrides — empty for the bundled-template
        // default path; populated when the Quick Start form's
        // MembershipEditor produced the roster.
        const QString modelProvider = memberMap.value(QStringLiteral("modelProvider")).toString();
        const QString modelName = memberMap.value(QStringLiteral("modelName")).toString();
        const QStringList allowedTools =
            memberMap.value(QStringLiteral("allowedTools")).toStringList();

        if (!m_membershipSvc.addProjectMember(folderId,
                                              resolvedAgentId,
                                              alias,
                                              isCoordinator,
                                              QStringLiteral("user"),
                                              QString(),  // addedByAgentId
                                              modelProvider,
                                              modelName,
                                              allowedTools)) {
            return rollbackAndFail(QStringLiteral("addProjectMember failed for alias: ") + alias);
        }
    }


    if (!database.commit()) {
        return rollbackAndFail(QStringLiteral("commit failed: ") + database.lastError().text());
    }

    qCInfo(verzetaUi) << "ProjectTemplateService: created project" << folderId << "name:" << name
                      << "members:" << effectiveMembers.size()
                      << "from template:" << (blank ? QStringLiteral("(blank)") : templateId);

    emit projectCreatedFromTemplate(folderId, name, effectiveMembers.size());
    return folderId;
}

// ---------------------------------------------------------------------------
// User templates — pin / save / delete
// ---------------------------------------------------------------------------

bool ProjectTemplateService::setTemplatePinned(const QString& templateId, bool pinned) {
    VERZETA_ASSERT_MAIN_THREAD();
    const int idx = m_indexById.value(templateId, -1);
    if (idx < 0) {
        qCWarning(verzetaUi) << "ProjectTemplateService::setTemplatePinned: unknown id"
                             << templateId;
        return false;
    }
    if (!m_templates.at(idx).value(QStringLiteral("isUserSaved")).toBool()) {
        // Built-ins are always on the landing — not pinnable.
        qCWarning(verzetaUi) << "ProjectTemplateService::setTemplatePinned: refusing to"
                             << "pin/unpin built-in template" << templateId;
        return false;
    }
    const bool already = m_pinnedIds.contains(templateId);
    if (pinned == already) {
        return true;  // idempotent no-op
    }
    if (pinned)
        m_pinnedIds.insert(templateId);
    else
        m_pinnedIds.remove(templateId);
    if (!savePinnedSet()) {
        // Roll the in-memory change back so memory + disk stay in sync.
        if (pinned)
            m_pinnedIds.remove(templateId);
        else
            m_pinnedIds.insert(templateId);
        return false;
    }
    emit catalogChanged();
    return true;
}

QString ProjectTemplateService::saveAsNewTemplate(const QString& sourceTemplateId,
                                                  const QVariantMap& edits) {
    VERZETA_ASSERT_MAIN_THREAD();

    const QString name = edits.value(QStringLiteral("name")).toString().trimmed();
    if (name.isEmpty()) {
        qCWarning(verzetaUi) << "ProjectTemplateService::saveAsNewTemplate: empty name";
        return {};
    }

    // Seed presentation fields (category / tag / banner art) from the
    // source template when one was given; otherwise neutral defaults.
    QVariantMap source;
    if (!sourceTemplateId.isEmpty()) {
        const int idx = m_indexById.value(sourceTemplateId, -1);
        if (idx >= 0)
            source = m_templates.at(idx);
    }
    const auto seeded = [&](const QString& key, const QVariant& fallback) {
        const QVariant s = source.value(key);
        return s.isValid() ? s : fallback;
    };

    // Mint a collision-free user-template id.
    QString newId;
    do {
        newId = QStringLiteral("user-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    } while (m_indexById.contains(newId));

    QVariantMap row;
    row.insert(QStringLiteral("id"), newId);
    row.insert(QStringLiteral("name"), name);
    row.insert(QStringLiteral("tagLabel"),
               seeded(QStringLiteral("tagLabel"), QStringLiteral("CUSTOM")));
    row.insert(QStringLiteral("category"),
               seeded(QStringLiteral("category"), QStringLiteral("discovery")));
    // The Quick Start form's design picker rides in via `edits`; fall
    // back to the source template's look, then a neutral default.
    const QVariant editGeom = edits.value(QStringLiteral("geometryKind"));
    const QVariant editHue = edits.value(QStringLiteral("baseHue"));
    row.insert(QStringLiteral("geometryKind"),
               (editGeom.isValid() && !editGeom.toString().isEmpty())
                   ? editGeom
                   : seeded(QStringLiteral("geometryKind"), QStringLiteral("circles")));
    row.insert(QStringLiteral("baseHue"),
               editHue.isValid() ? editHue : seeded(QStringLiteral("baseHue"), 210));
    row.insert(QStringLiteral("scenario"), edits.value(QStringLiteral("scenario")).toString());
    row.insert(QStringLiteral("goal"), edits.value(QStringLiteral("goal")).toString());
    row.insert(QStringLiteral("description"),
               edits.value(QStringLiteral("description")).toString());
    // User templates keep every member (coordinator included) in
    // members[], identified by agentId — there is no top-level
    // `coordinator` name field. createProjectFromTemplate's
    // customisations.members path and templateRoster both handle this.
    row.insert(QStringLiteral("coordinator"), QString());
    row.insert(QStringLiteral("members"), edits.value(QStringLiteral("members")).toList());
    row.insert(QStringLiteral("seedDocuments"), QVariantList{});

    // Write the JSON file atomically.
    QDir().mkpath(m_userTemplateDir);
    const QString path = m_userTemplateDir + QLatin1Char('/') + newId + QStringLiteral(".json");
    const QByteArray bytes =
        QJsonDocument(QJsonObject::fromVariantMap(row)).toJson(QJsonDocument::Indented);
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qCWarning(verzetaUi) << "ProjectTemplateService::saveAsNewTemplate: cannot write" << path
                             << "—" << f.errorString();
        return {};
    }
    f.write(bytes);
    if (!f.commit()) {
        qCWarning(verzetaUi) << "ProjectTemplateService::saveAsNewTemplate: commit failed"
                             << "for" << path << "—" << f.errorString();
        return {};
    }

    // Add to the live catalog.
    row.insert(QStringLiteral("isUserSaved"), true);
    m_indexById.insert(newId, m_templates.size());
    m_templates.append(row);

    qCInfo(verzetaUi) << "ProjectTemplateService: saved user template" << newId << "name:" << name;
    emit catalogChanged();
    return newId;
}

bool ProjectTemplateService::deleteUserTemplate(const QString& templateId) {
    VERZETA_ASSERT_MAIN_THREAD();
    const int idx = m_indexById.value(templateId, -1);
    if (idx < 0) {
        qCWarning(verzetaUi) << "ProjectTemplateService::deleteUserTemplate: unknown id"
                             << templateId;
        return false;
    }
    if (!m_templates.at(idx).value(QStringLiteral("isUserSaved")).toBool()) {
        qCWarning(verzetaUi) << "ProjectTemplateService::deleteUserTemplate: refusing to"
                             << "delete built-in template" << templateId;
        return false;
    }

    // Remove the JSON file (best-effort — the catalog row is the
    // source of truth; a stale file would just be re-skipped on load).
    QFile::remove(m_userTemplateDir + QLatin1Char('/') + templateId + QStringLiteral(".json"));

    // Drop the catalog row, then rebuild the id → index map (every
    // index after the erased one shifts down by one).
    m_templates.removeAt(idx);
    m_indexById.clear();
    for (int i = 0; i < m_templates.size(); ++i) {
        m_indexById.insert(m_templates.at(i).value(QStringLiteral("id")).toString(), i);
    }

    // Drop any pin and persist.
    if (m_pinnedIds.remove(templateId)) {
        savePinnedSet();
    }

    qCInfo(verzetaUi) << "ProjectTemplateService: deleted user template" << templateId;
    emit catalogChanged();
    return true;
}
