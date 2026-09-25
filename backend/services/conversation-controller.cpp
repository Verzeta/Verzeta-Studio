// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file conversation-controller.cpp
 * @brief Implementation of the `Conversations` QML singleton. Owns
 *        conversation + folder + group-conversation CRUD. Bodies
 *        migrated verbatim from the deleted Chat::ConversationManager
 *        collaborator, with the pure CRUD methods (newConversation,
 *        deleteConversation, renameConversation, clearAllConversations)
 *        absorbed from ChatController.
 * @layer Service
 * @dependencies ConversationService, ModelRouter, FileService
 *               (optional), MembershipService (optional),
 *               models/conversation.h, models/member.h,
 *               models/llm-config.h.
 *
 * Coordination with ChatController is through signals, not direct
 * calls. Conversations emits conversationCreated / conversationDeleted
 * / conversationRenamed, and ChatController subscribes to update its
 * active-conversation state when needed.
 */

#include "conversation-controller.h"

#include "../models/conversation.h"
#include "../models/llm-config.h"
#include "../models/member.h"
#include "../utils/logger.h"
#include "../utils/thread-discipline.h"
#include "conversation-service.h"
#include "file-service.h"
#include "membership-service.h"
#include "model-router.h"

#include <QFileInfo>
#include <QSet>
#include <QUrl>

ConversationController::ConversationController(ConversationService& convSvc,
                                               ModelRouter& router,
                                               QObject* parent)
    : QObject(parent), m_convSvc(convSvc), m_router(router) {
    qCInfo(verzetaUi) << "ConversationController initialized";
}

ConversationController::~ConversationController() {
    qCInfo(verzetaUi) << "ConversationController destroyed";
}

void ConversationController::setFileService(FileService* svc) {
    VERZETA_ASSERT_MAIN_THREAD();
    m_fileSvc = svc;
    qCInfo(verzetaUi) << "ConversationController: FileService" << (svc ? "attached" : "detached");
}

void ConversationController::setMembershipService(MembershipService* svc) {
    VERZETA_ASSERT_MAIN_THREAD();
    m_membership = svc;
    qCInfo(verzetaUi) << "ConversationController: MembershipService"
                      << (svc ? "attached" : "detached");
}

// ---------------------------------------------------------------------------
// Conversation CRUD
// ---------------------------------------------------------------------------

QString ConversationController::newConversation(const QString& title) {
    VERZETA_ASSERT_MAIN_THREAD();
    const QString id = m_convSvc.createConversation(title);
    if (id.isEmpty()) {
        emit errorOccurred(QStringLiteral("Failed to create a new conversation."));
        return {};
    }

    // Stamp the router's current provider/model into the new
    // conversation's llm_config so re-opening it later restores the
    // same selection — same semantics pre-extraction ChatController
    // enforced in newConversation + sendMessage's auto-create path.
    if (!m_router.activeModelName().isEmpty()) {
        LlmConfig cfg;
        cfg.providerId = m_router.activeProviderId();
        cfg.modelName = m_router.activeModelName();
        cfg.stream = true;
        m_convSvc.updateLlmConfig(id, cfg);
    }

    emit conversationCreated(id);
    qCInfo(verzetaUi) << "ConversationController: new conversation" << id;
    return id;
}

QString ConversationController::newConversationWithAgent(const QString& agentId,
                                                         const QString& title) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (agentId.isEmpty()) {
        emit errorOccurred(QStringLiteral("Cannot create the chat because no agent was selected."));
        return {};
    }

    const QString convTitle = title.trimmed().isEmpty() ? QStringLiteral("New Chat") : title;
    const QString id = m_convSvc.createConversation(convTitle);
    if (id.isEmpty()) {
        emit errorOccurred(QStringLiteral("Failed to create a new conversation."));
        return {};
    }

    if (!m_router.activeModelName().isEmpty()) {
        LlmConfig cfg;
        cfg.providerId = m_router.activeProviderId();
        cfg.modelName = m_router.activeModelName();
        cfg.stream = true;
        m_convSvc.updateLlmConfig(id, cfg);
    }
    m_convSvc.updatePrimaryAgent(id, agentId);

    emit conversationCreated(id);
    return id;
}

void ConversationController::deleteConversation(const QString& id) {
    VERZETA_ASSERT_MAIN_THREAD();
    // Fire the pre-delete signal FIRST so subscribers (notably
    // ChatController) can cancel any in-flight request that targets
    // this conversation — the in-flight request's final DB write
    // must land on a still-valid FK. Skipping the pre-signal would
    // let the provider thread queue a chunk/finish against a row
    // that vanishes before the main thread processes it.
    emit conversationAboutToBeDeleted(id);

    // ConversationService::deleteConversation emits conversationDeleted,
    // which propagates to ConversationListModel (row removal),
    // MessageService::onConversationDeleted (streaming abort),
    // TaskRunner::onConversationDeleted (plan cleanup), and
    // PlanService::deletePlansForConversation. Our own
    // conversationDeleted signal is the high-level coordination
    // channel — ChatController subscribes to clear its active state
    // if the deleted id was active.
    m_convSvc.deleteConversation(id);
    emit conversationDeleted(id);
}

bool ConversationController::renameConversation(const QString& id, const QString& title) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (title.trimmed().isEmpty()) {
        return false;
    }
    if (!m_convSvc.renameConversation(id, title)) {
        emit errorOccurred(QStringLiteral("Failed to rename the conversation."));
        return false;
    }
    emit conversationRenamed(id, title);
    qCInfo(verzetaUi) << "Conversation renamed:" << id << "->" << title;
    return true;
}

void ConversationController::clearAllConversations() {
    VERZETA_ASSERT_MAIN_THREAD();
    const QList<Conversation> all = m_convSvc.listAllConversations();
    for (const Conversation& c : all) {
        // Same pre/post signal pair per row as deleteConversation so
        // ChatController can short-circuit in-flight work before the
        // active conversation row is wiped.
        emit conversationAboutToBeDeleted(c.id);
        m_convSvc.deleteConversation(c.id);
        emit conversationDeleted(c.id);
    }
    qCInfo(verzetaUi) << "All conversations cleared";
}

// ---------------------------------------------------------------------------
// Folder CRUD
// ---------------------------------------------------------------------------

QString ConversationController::createFolder(const QString& name) {
    VERZETA_ASSERT_MAIN_THREAD();
    return m_convSvc.createFolder(name);
}

bool ConversationController::moveToFolder(const QString& convId, const QString& folderId) {
    VERZETA_ASSERT_MAIN_THREAD();
    return m_convSvc.moveToFolder(convId, folderId);
}

QVariantMap ConversationController::folderInfo(const QString& folderId) const {
    VERZETA_ASSERT_MAIN_THREAD();
    QVariantMap map;
    const auto f = const_cast<ConversationService&>(m_convSvc).getFolder(folderId);
    if (!f.has_value())
        return map;
    map[QStringLiteral("id")] = f->id;
    map[QStringLiteral("name")] = f->name;
    map[QStringLiteral("parentId")] = f->parentId;
    map[QStringLiteral("folderType")] = f->folderType;
    map[QStringLiteral("goal")] = f->goal;
    map[QStringLiteral("description")] = f->description;
    map[QStringLiteral("agentIds")] = f->agentIds;
    map[QStringLiteral("acnEnabled")] =
        const_cast<ConversationService&>(m_convSvc).folderAcnEnabled(folderId);
    return map;
}

bool ConversationController::setFolderAcnEnabled(const QString& folderId, bool enabled) {
    VERZETA_ASSERT_MAIN_THREAD();
    return m_convSvc.setFolderAcnEnabled(folderId, enabled);
}

bool ConversationController::updateFolderMetadata(const QString& folderId,
                                                  const QString& folderType,
                                                  const QString& goal,
                                                  const QString& description,
                                                  const QStringList& agentIds) {
    VERZETA_ASSERT_MAIN_THREAD();
    return m_convSvc.updateFolderMetadata(folderId, folderType, goal, description, agentIds);
}

bool ConversationController::renameFolder(const QString& folderId, const QString& newName) {
    VERZETA_ASSERT_MAIN_THREAD();
    return m_convSvc.renameFolder(folderId, newName);
}

bool ConversationController::deleteFolder(const QString& folderId) {
    VERZETA_ASSERT_MAIN_THREAD();
    return m_convSvc.deleteFolder(folderId);
}

QVariantList ConversationController::listProjectFolders() const {
    VERZETA_ASSERT_MAIN_THREAD();
    QVariantList out;
    QList<QString> stack;
    stack.append(QString());  // root
    while (!stack.isEmpty()) {
        const QString parent = stack.takeFirst();
        const QList<Folder> children =
            const_cast<ConversationService&>(m_convSvc).listFolders(parent);
        for (const Folder& f : children) {
            if (f.isProject()) {
                QVariantMap m;
                m[QStringLiteral("id")] = f.id;
                m[QStringLiteral("name")] = f.name;
                m[QStringLiteral("type")] = f.folderType;
                out.append(m);
            }
            stack.append(f.id);
        }
    }
    return out;
}

// ---------------------------------------------------------------------------
// Project documents
// ---------------------------------------------------------------------------

QVariantList ConversationController::projectDocuments(const QString& folderId) const {
    VERZETA_ASSERT_MAIN_THREAD();
    QVariantList out;
    if (!m_fileSvc)
        return out;
    const auto f = const_cast<ConversationService&>(m_convSvc).getFolder(folderId);
    if (!f.has_value())
        return out;
    const QStringList names = m_fileSvc->listProjectDocuments(f->id, f->name);
    const QString dir = m_fileSvc->projectDocsDir(f->id, f->name);
    for (const QString& n : names) {
        const QString full = dir + QStringLiteral("/") + n;
        QVariantMap m;
        m[QStringLiteral("name")] = n;
        m[QStringLiteral("path")] = full;
        m[QStringLiteral("size")] = QFileInfo(full).size();
        out.append(m);
    }
    return out;
}

QString ConversationController::addProjectDocument(const QString& folderId,
                                                   const QString& sourcePath) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (!m_fileSvc)
        return {};
    const auto f = m_convSvc.getFolder(folderId);
    if (!f.has_value())
        return {};
    QString src = sourcePath;
    if (src.startsWith(QStringLiteral("file://"))) {
        src = QUrl(src).toLocalFile();
    }
    return m_fileSvc->addProjectDocument(f->id, f->name, src);
}

bool ConversationController::removeProjectDocument(const QString& folderId,
                                                   const QString& fileName) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (!m_fileSvc)
        return false;
    const auto f = m_convSvc.getFolder(folderId);
    if (!f.has_value())
        return false;
    return m_fileSvc->removeProjectDocument(f->id, f->name, fileName);
}

// ---------------------------------------------------------------------------
// Group conversations
// ---------------------------------------------------------------------------

QString ConversationController::newGroupConversation(const QString& title,
                                                     const QVariantList& members,
                                                     const QString& folderId) {
    VERZETA_ASSERT_MAIN_THREAD();

    if (members.isEmpty() || !m_membership) {
        emit errorOccurred(QStringLiteral("A group chat needs at least one member."));
        return {};
    }

    QStringList agentIdsLegacy;
    agentIdsLegacy.reserve(members.size());
    QList<Member> memberList;
    memberList.reserve(members.size());
    for (const QVariant& v : members) {
        const QVariantMap m = v.toMap();
        agentIdsLegacy.append(m.value(QStringLiteral("agentId")).toString());
        Member mm;
        mm.agentId = m.value(QStringLiteral("agentId")).toString();
        mm.alias = m.value(QStringLiteral("alias")).toString();
        mm.isCoordinator = m.value(QStringLiteral("isCoordinator")).toBool();
        mm.modelProvider = m.value(QStringLiteral("modelProvider")).toString();
        mm.modelName = m.value(QStringLiteral("modelName")).toString();
        mm.allowedTools = m.value(QStringLiteral("allowedTools")).toStringList();
        memberList.append(mm);
    }
    const QString id = m_convSvc.createGroupConversation(title, agentIdsLegacy, folderId);
    if (id.isEmpty()) {
        emit errorOccurred(QStringLiteral("Failed to create the group conversation."));
        return {};
    }

    m_membership->setConversationMembers(id, memberList);

    if (!m_router.activeModelName().isEmpty()) {
        LlmConfig cfg;
        cfg.providerId = m_router.activeProviderId();
        cfg.modelName = m_router.activeModelName();
        cfg.stream = true;
        m_convSvc.updateLlmConfig(id, cfg);
    }

    emit conversationCreated(id);
    return id;
}

QString ConversationController::openDirectChatWithMember(const QString& folderId,
                                                         const QString& agentId,
                                                         const QString& alias) {
    VERZETA_ASSERT_MAIN_THREAD();

    if (agentId.isEmpty() || alias.isEmpty()) {
        // Pre-extraction behaviour was a silent return of empty id.
        // Preserve — do not emit an error.
        return {};
    }

    const QString titleTag = QStringLiteral("Chat with @%1")
                                 .arg(alias.trimmed().replace(QLatin1Char(' '), QLatin1Char('_')));

    const QList<Conversation> folderConvs = m_convSvc.listConversations(folderId);
    for (const Conversation& c : folderConvs) {
        if (c.isGroup)
            continue;
        if (c.primaryAgentId == agentId && c.title == titleTag) {
            // Reused existing conversation — still emit so callers can
            // treat both reused and newly-created ids uniformly.
            emit conversationCreated(c.id);
            return c.id;
        }
    }

    const QString id = m_convSvc.createConversation(titleTag, folderId);
    if (id.isEmpty()) {
        emit errorOccurred(QStringLiteral("Failed to create the direct chat."));
        return {};
    }

    if (!m_router.activeModelName().isEmpty()) {
        LlmConfig cfg;
        cfg.providerId = m_router.activeProviderId();
        cfg.modelName = m_router.activeModelName();
        cfg.stream = true;
        m_convSvc.updateLlmConfig(id, cfg);
    }
    m_convSvc.updatePrimaryAgent(id, agentId);
    m_convSvc.setConversationMemberAlias(id, alias);

    emit conversationCreated(id);
    return id;
}

int ConversationController::createIndividualChatsForProject(const QString& folderId) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (folderId.isEmpty() || !m_membership)
        return 0;

    const QList<Member> members = m_membership->projectMembers(folderId);
    if (members.isEmpty())
        return 0;

    const QList<Conversation> folderConvs = m_convSvc.listConversations(folderId);
    QSet<QString> existingTags;
    existingTags.reserve(folderConvs.size());
    for (const Conversation& c : folderConvs) {
        if (c.isGroup)
            continue;
        existingTags.insert(c.primaryAgentId + QLatin1Char('|') + c.title);
    }

    LlmConfig defaultCfg;
    const bool stampLlmConfig = !m_router.activeModelName().isEmpty();
    if (stampLlmConfig) {
        defaultCfg.providerId = m_router.activeProviderId();
        defaultCfg.modelName = m_router.activeModelName();
        defaultCfg.stream = true;
    }

    int count = 0;
    for (const Member& m : members) {
        const QString tag = QStringLiteral("Chat with @%1")
                                .arg(m.alias.trimmed().replace(QLatin1Char(' '), QLatin1Char('_')));

        if (existingTags.contains(m.agentId + QLatin1Char('|') + tag))
            continue;

        const QString id = m_convSvc.createConversation(tag, folderId);
        if (id.isEmpty())
            continue;

        if (stampLlmConfig) {
            m_convSvc.updateLlmConfig(id, defaultCfg);
        }
        m_convSvc.updatePrimaryAgent(id, m.agentId);
        m_convSvc.setConversationMemberAlias(id, m.alias);
        ++count;
    }

    return count;
}

QString ConversationController::createGroupChatForProject(const QString& folderId) {
    VERZETA_ASSERT_MAIN_THREAD();

    if (folderId.isEmpty() || !m_membership)
        return {};

    const auto folder = m_convSvc.getFolder(folderId);
    if (!folder.has_value())
        return {};

    const QList<Member> members = m_membership->projectMembers(folderId);
    if (members.size() < 2) {
        emit errorOccurred(QStringLiteral("A group chat needs at least two members"));
        return {};
    }

    QVariantList memberMaps;
    memberMaps.reserve(members.size());
    for (const Member& m : members) {
        QVariantMap map;
        map[QStringLiteral("agentId")] = m.agentId;
        map[QStringLiteral("alias")] = m.alias;
        map[QStringLiteral("isCoordinator")] = m.isCoordinator;
        map[QStringLiteral("modelProvider")] = m.modelProvider;
        map[QStringLiteral("modelName")] = m.modelName;
        map[QStringLiteral("allowedTools")] = m.allowedTools;
        memberMaps.append(map);
    }

    const QString title = QStringLiteral("Team chat: %1").arg(folder->name);
    return newGroupConversation(title, memberMaps, folderId);
}

// ---------------------------------------------------------------------------
// Parameterised queries
// ---------------------------------------------------------------------------

bool ConversationController::isGroup(const QString& convId) const {
    VERZETA_ASSERT_MAIN_THREAD();
    if (convId.isEmpty())
        return false;
    const auto conv = m_convSvc.getConversation(convId);
    return conv.has_value() && conv->isGroup;
}

QString ConversationController::folderIdOf(const QString& convId) const {
    VERZETA_ASSERT_MAIN_THREAD();
    if (convId.isEmpty())
        return {};
    const auto conv = m_convSvc.getConversation(convId);
    return conv.has_value() ? conv->folderId : QString();
}

QString ConversationController::conversationMemberAlias(const QString& convId) const {
    VERZETA_ASSERT_MAIN_THREAD();
    if (convId.isEmpty())
        return {};
    const auto conv = m_convSvc.getConversation(convId);
    return conv.has_value() ? conv->memberAlias : QString();
}

QVariantList ConversationController::groupMembers(const QString& convId) const {
    VERZETA_ASSERT_MAIN_THREAD();
    QVariantList list;
    if (convId.isEmpty() || !m_membership)
        return list;

    const auto conv = m_convSvc.getConversation(convId);
    if (!conv.has_value() || !conv->isGroup)
        return list;

    const QList<Member> members = m_membership->conversationMembers(convId);
    for (const Member& mem : members) {
        QVariantMap m;
        m[QStringLiteral("agentId")] = mem.agentId;
        m[QStringLiteral("alias")] = mem.alias;
        m[QStringLiteral("isCoordinator")] = mem.isCoordinator;
        m[QStringLiteral("name")] = mem.agentName;
        m[QStringLiteral("description")] = mem.agentDescription;
        m[QStringLiteral("iconName")] = mem.agentIconName;
        list.append(m);
    }
    return list;
}

QVariantList ConversationController::conversationsInFolder(const QString& folderId) {
    VERZETA_ASSERT_MAIN_THREAD();
    QVariantList out;
    const auto convs = m_convSvc.listConversations(folderId);
    for (const Conversation& c : convs) {
        QVariantMap m;
        m[QStringLiteral("id")] = c.id;
        m[QStringLiteral("title")] = c.title;
        m[QStringLiteral("isGroup")] = c.isGroup;
        m[QStringLiteral("updatedAtMs")] = c.updatedAt.toMSecsSinceEpoch();
        out.append(m);
    }
    return out;
}


bool ConversationController::heartbeatAutoSurface(const QString& convId) const {
    VERZETA_ASSERT_MAIN_THREAD();
    if (convId.isEmpty())
        return false;
    const auto conv = m_convSvc.getConversation(convId);
    return conv.has_value() && conv->allowHeartbeatAutoSurface();
}

int ConversationController::heartbeatAutoSurfaceMaxPerDay(const QString& convId) const {
    VERZETA_ASSERT_MAIN_THREAD();
    if (convId.isEmpty())
        return 1;
    const auto conv = m_convSvc.getConversation(convId);
    if (!conv.has_value())
        return 1;
    return conv->heartbeatAutoSurfaceMaxPerDay();
}

bool ConversationController::setHeartbeatAutoSurface(const QString& convId,
                                                     bool allow,
                                                     int maxPerDay) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (convId.isEmpty())
        return false;
    return m_convSvc.setHeartbeatAutoSurface(convId, allow, maxPerDay);
}
