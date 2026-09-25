// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file skill-service.cpp
 * @brief Implementation of SkillService. See header for the API
 *        contract and the project's skills documentation for storage
 *        layout, manifest schemas, and resolver semantics.
 * @layer Service
 * @dependencies Qt6::Core, SkillParser, SkillHash, ConversationService.
 */

#include "skill-service.h"

#include "../models/conversation.h"
#include "../utils/logger.h"
#include "../utils/skill-archive-extractor.h"
#include "../utils/skill-hash.h"
#include "../utils/skill-parser.h"
#include "conversation-service.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QStandardPaths>
#include <QUuid>

namespace {
constexpr int kManifestSchemaVersion = 1;
}

SkillService::SkillService(ConversationService& convs, QObject* parent)
    : QObject(parent), m_convs(convs) {}

SkillService::~SkillService() {
    shutdown();
}

void SkillService::setAppDataRootForTesting(const QString& absRoot) {
    m_appDataRoot = absRoot;
}

void SkillService::initialize() {
    if (m_initialized)
        return;
    if (m_appDataRoot.isEmpty()) {
        m_appDataRoot = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    }
    m_skillsRoot = m_appDataRoot + QStringLiteral("/skills");
    m_installedDir = m_skillsRoot + QStringLiteral("/installed");
    m_quarantineDir = m_skillsRoot + QStringLiteral("/quarantine");
    m_manifestsDir = m_skillsRoot + QStringLiteral("/manifests");
    m_installedManifestPath = m_manifestsDir + QStringLiteral("/installed-skills.json");
    m_preferredManifestPath = m_manifestsDir + QStringLiteral("/preferred-skills.json");
    m_reviewManifestPath = m_manifestsDir + QStringLiteral("/review-state.json");

    ensureDirsExist();
    loadInstalledManifest();
    loadReviewManifest();
    loadPreferredManifest();

    // Cross-link review state into the in-memory Skill rows so callers
    // can filter approved-only without a second lookup.
    for (auto it = m_installed.begin(); it != m_installed.end(); ++it) {
        const auto rIt = m_review.constFind(it.key());
        if (rIt != m_review.constEnd() && rIt->contentHashSha256 == it->contentHashSha256 &&
            rIt->version == it->version) {
            it->reviewState = rIt->state;
        } else {
            it->reviewState = QStringLiteral("unreviewed");
        }
    }

    m_initialized = true;
}

void SkillService::shutdown() {
    m_installed.clear();
    m_review.clear();
    m_preferred.clear();
    m_initialized = false;
}

void SkillService::ensureDirsExist() {
    QDir().mkpath(m_installedDir);
    QDir().mkpath(m_quarantineDir);
    QDir().mkpath(m_manifestsDir);
}

// ---------------------------------------------------------------------------
// Manifest I/O
// ---------------------------------------------------------------------------

void SkillService::loadInstalledManifest() {
    m_installed.clear();
    QFile f(m_installedManifestPath);
    if (!f.exists())
        return;
    if (!f.open(QIODevice::ReadOnly)) {
        qCWarning(verzetaUi) << "SkillService: cannot read installed-skills.json:"
                             << f.errorString();
        return;
    }
    const QByteArray bytes = f.readAll();
    f.close();

    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        qCWarning(verzetaUi) << "SkillService: corrupt installed-skills.json (treating as empty):"
                             << pe.errorString();
        return;
    }
    const QJsonObject root = doc.object();
    if (root.value(QStringLiteral("schema_version")).toInt() > kManifestSchemaVersion) {
        emit errorOccurred(QStringLiteral("installed-skills.json version too new for this build"));
        return;
    }
    const QJsonArray skills = root.value(QStringLiteral("skills")).toArray();
    for (const QJsonValue& v : skills) {
        const QJsonObject so = v.toObject();
        Skill s;
        s.id = so.value(QStringLiteral("id")).toString();
        s.source = so.value(QStringLiteral("source")).toString();
        s.sourceUrl = so.value(QStringLiteral("source_url")).toString();
        s.version = so.value(QStringLiteral("version")).toString();
        s.installPath = so.value(QStringLiteral("install_path")).toString();
        s.installedAtMs =
            static_cast<qint64>(so.value(QStringLiteral("installed_at_ms")).toDouble());
        s.updatedAtMs = static_cast<qint64>(so.value(QStringLiteral("updated_at_ms")).toDouble());
        s.contentHashSha256 = so.value(QStringLiteral("content_hash_sha256")).toString();
        const QJsonObject mk = so.value(QStringLiteral("manifest_keys")).toObject();
        s.displayName = mk.value(QStringLiteral("display_name")).toString();
        if (s.displayName.isEmpty())
            s.displayName = s.id;  // pre-displayName manifests
        s.description = mk.value(QStringLiteral("description")).toString();
        const QJsonArray tagsArr = mk.value(QStringLiteral("tags")).toArray();
        for (const QJsonValue& tv : tagsArr)
            s.tags.append(tv.toString());
        const QJsonArray toolsArr = mk.value(QStringLiteral("tools")).toArray();
        for (const QJsonValue& tv : toolsArr)
            s.declaredTools.append(tv.toString());
        const QJsonArray warrArr = so.value(QStringLiteral("warnings")).toArray();
        for (const QJsonValue& wv : warrArr) {
            s.warnings.append(SkillWarning::fromJson(wv.toObject()));
        }
        if (s.id.isEmpty())
            continue;
        m_installed.insert(s.id, s);
    }
}

void SkillService::loadReviewManifest() {
    m_review.clear();
    QFile f(m_reviewManifestPath);
    if (!f.exists())
        return;
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QByteArray bytes = f.readAll();
    f.close();
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        qCWarning(verzetaUi) << "SkillService: corrupt review-state.json (treating as empty)";
        return;
    }
    const QJsonObject root = doc.object();
    const QJsonArray decs = root.value(QStringLiteral("decisions")).toArray();
    for (const QJsonValue& v : decs) {
        const auto d = SkillReviewDecision::fromJson(v.toObject());
        if (d.skillId.isEmpty())
            continue;
        m_review.insert(d.skillId, d);
    }
}

void SkillService::loadPreferredManifest() {
    m_preferred.clear();
    QFile f(m_preferredManifestPath);
    if (!f.exists())
        return;
    if (!f.open(QIODevice::ReadOnly))
        return;
    const QByteArray bytes = f.readAll();
    f.close();
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        qCWarning(verzetaUi) << "SkillService: corrupt preferred-skills.json (treating as empty)";
        return;
    }
    const QJsonObject root = doc.object();
    const QJsonArray scopes = root.value(QStringLiteral("scopes")).toArray();
    for (const QJsonValue& v : scopes) {
        m_preferred.append(PreferredSkillsScope::fromJson(v.toObject()));
    }
}

bool SkillService::writeInstalledManifest() {
    QJsonObject root;
    root.insert(QStringLiteral("schema_version"), kManifestSchemaVersion);
    QJsonArray skills;
    for (const Skill& s : m_installed) {
        QJsonObject so;
        so.insert(QStringLiteral("id"), s.id);
        so.insert(QStringLiteral("source"), s.source);
        so.insert(QStringLiteral("source_url"), s.sourceUrl);
        so.insert(QStringLiteral("version"), s.version);
        so.insert(QStringLiteral("install_path"), s.installPath);
        so.insert(QStringLiteral("installed_at_ms"),
                  QJsonValue::fromVariant(QVariant::fromValue(s.installedAtMs)));
        so.insert(QStringLiteral("updated_at_ms"),
                  QJsonValue::fromVariant(QVariant::fromValue(s.updatedAtMs)));
        so.insert(QStringLiteral("content_hash_sha256"), s.contentHashSha256);
        QJsonObject mk;
        mk.insert(QStringLiteral("display_name"), s.displayName);
        mk.insert(QStringLiteral("description"), s.description);
        QJsonArray ta;
        for (const QString& t : s.tags)
            ta.append(t);
        mk.insert(QStringLiteral("tags"), ta);
        QJsonArray ts;
        for (const QString& t : s.declaredTools)
            ts.append(t);
        mk.insert(QStringLiteral("tools"), ts);
        so.insert(QStringLiteral("manifest_keys"), mk);
        QJsonArray warr;
        for (const SkillWarning& w : s.warnings)
            warr.append(w.toJson());
        so.insert(QStringLiteral("warnings"), warr);
        skills.append(so);
    }
    root.insert(QStringLiteral("skills"), skills);
    QString err;
    if (!writeJsonAtomic(m_installedManifestPath, root, &err)) {
        emit errorOccurred(QStringLiteral("write installed-skills.json failed: %1").arg(err));
        return false;
    }
    return true;
}

bool SkillService::writeReviewManifest() {
    QJsonObject root;
    root.insert(QStringLiteral("schema_version"), kManifestSchemaVersion);
    QJsonArray decs;
    for (const auto& d : m_review)
        decs.append(d.toJson());
    root.insert(QStringLiteral("decisions"), decs);
    QString err;
    if (!writeJsonAtomic(m_reviewManifestPath, root, &err)) {
        emit errorOccurred(QStringLiteral("write review-state.json failed: %1").arg(err));
        return false;
    }
    return true;
}

bool SkillService::writePreferredManifest() {
    QJsonObject root;
    root.insert(QStringLiteral("schema_version"), kManifestSchemaVersion);
    QJsonArray scopes;
    for (const PreferredSkillsScope& p : m_preferred)
        scopes.append(p.toJson());
    root.insert(QStringLiteral("scopes"), scopes);
    QString err;
    if (!writeJsonAtomic(m_preferredManifestPath, root, &err)) {
        emit errorOccurred(QStringLiteral("write preferred-skills.json failed: %1").arg(err));
        return false;
    }
    return true;
}

bool SkillService::writeJsonAtomic(const QString& absPath,
                                   const QJsonObject& obj,
                                   QString* outError) {
    QSaveFile out(absPath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (outError)
            *outError = out.errorString();
        return false;
    }
    const QByteArray bytes = QJsonDocument(obj).toJson(QJsonDocument::Indented);
    if (out.write(bytes) != bytes.size()) {
        if (outError)
            *outError = out.errorString();
        out.cancelWriting();
        return false;
    }
    if (!out.commit()) {
        if (outError)
            *outError = out.errorString();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Library queries
// ---------------------------------------------------------------------------

int SkillService::approvedCount() const {
    int n = 0;
    for (const Skill& s : m_installed)
        if (s.isApproved())
            ++n;
    return n;
}

QVariantMap SkillService::skillToVariantMap(const Skill& s) const {
    QVariantMap m;
    m.insert(QStringLiteral("id"), s.id);
    m.insert(QStringLiteral("displayName"), s.displayName.isEmpty() ? s.id : s.displayName);
    m.insert(QStringLiteral("source"), s.source);
    m.insert(QStringLiteral("sourceUrl"), s.sourceUrl);
    m.insert(QStringLiteral("version"), s.version);
    m.insert(QStringLiteral("description"), s.description);
    m.insert(QStringLiteral("tags"), s.tags);
    m.insert(QStringLiteral("declaredTools"), s.declaredTools);
    m.insert(QStringLiteral("contentHashShort"), s.contentHashSha256.left(12));
    m.insert(QStringLiteral("contentHashSha256"), s.contentHashSha256);
    m.insert(QStringLiteral("installedAtMs"), s.installedAtMs);
    m.insert(QStringLiteral("updatedAtMs"), s.updatedAtMs);
    m.insert(QStringLiteral("installPath"), m_appDataRoot + QStringLiteral("/") + s.installPath);
    m.insert(QStringLiteral("reviewState"), s.reviewState);
    QVariantList warr;
    for (const SkillWarning& w : s.warnings) {
        QVariantMap wm;
        wm.insert(QStringLiteral("regexName"), w.regexName);
        wm.insert(QStringLiteral("fileRelativePath"), w.fileRelativePath);
        wm.insert(QStringLiteral("lineNumber"), w.lineNumber);
        wm.insert(QStringLiteral("matchedExcerpt"), w.matchedExcerpt);
        warr.append(wm);
    }
    m.insert(QStringLiteral("warnings"), warr);
    return m;
}

QVariantList SkillService::installedSkills() const {
    QVariantList out;
    for (const Skill& s : m_installed)
        out.append(skillToVariantMap(s));
    return out;
}

QVariantList SkillService::approvedSkills() const {
    QVariantList out;
    for (const Skill& s : m_installed) {
        if (s.isApproved())
            out.append(skillToVariantMap(s));
    }
    return out;
}

QVariantMap SkillService::skillDetails(const QString& skillId) const {
    const auto it = m_installed.constFind(skillId);
    if (it == m_installed.constEnd())
        return {};
    return skillToVariantMap(*it);
}

Skill SkillService::skillById(const QString& skillId) const {
    return m_installed.value(skillId);
}

QString SkillService::installPathFor(const QString& skillId) const {
    const auto it = m_installed.constFind(skillId);
    if (it == m_installed.constEnd())
        return {};
    return m_appDataRoot + QStringLiteral("/") + it->installPath;
}

QString SkillService::readSkillFile(const QString& skillId, const QString& relativePath) const {
    const QString root = installPathFor(skillId);
    if (root.isEmpty())
        return {};
    if (relativePath.contains(QStringLiteral("..")))
        return {};
    if (relativePath.startsWith(QLatin1Char('/')))
        return {};
    QFile f(root + QStringLiteral("/") + relativePath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return {};
    const QByteArray bytes = f.readAll();
    f.close();
    return QString::fromUtf8(bytes);
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

QString SkillService::importSkillFolder(const QString& folderPath, const QString& idHint) {
    if (!m_initialized)
        return QStringLiteral("skills are not ready yet; try again in a moment");

    SkillParser::ParseResult pr = SkillParser::parseSkillFolder(folderPath, idHint);
    if (!pr.success) {
        // Quarantine: copy a snapshot of the bad folder so the user can
        // inspect why we rejected it. Best-effort — failures here are
        // logged but don't block the original error from surfacing.
        const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
        const QString basename = QFileInfo(folderPath).baseName();
        const QString qroot =
            m_quarantineDir + QStringLiteral("/") + ts + QStringLiteral("-") + basename;
        QString cpErr;
        copyFolderRecursive(folderPath, qroot, &cpErr);
        return pr.error;
    }

    Skill s = pr.skill;

    // Prepare staging install dir.
    const QString installRel = QStringLiteral("skills/installed/") + s.id;
    const QString installAbs = m_appDataRoot + QStringLiteral("/") + installRel;

    // Re-import path: existing dir replaced atomically (remove +
    // recursive copy from source).
    if (QFileInfo::exists(installAbs)) {
        if (!removeFolderRecursive(installAbs)) {
            return QStringLiteral("could not remove the existing skill folder: %1").arg(installAbs);
        }
    }
    QString cpErr;
    if (!copyFolderRecursive(folderPath, installAbs, &cpErr)) {
        return QStringLiteral("install copy failed: %1").arg(cpErr);
    }

    // Compute deterministic hash AFTER the copy lands so we hash the
    // installed bytes, not the source (any platform-specific copy
    // mtime quirks are eliminated — we never feed mtime into the hash
    // anyway, but hashing the canonical install copy is the
    // lower-confusion choice).
    QString hashErr;
    s.contentHashSha256 = SkillHash::computeFolderHash(installAbs, &hashErr);
    if (s.contentHashSha256.isEmpty()) {
        // Hash failed — back out the install.
        removeFolderRecursive(installAbs);
        return QStringLiteral("could not compute the skill checksum: %1").arg(hashErr);
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    s.installPath = installRel;
    s.installedAtMs = nowMs;
    s.updatedAtMs = nowMs;

    // Determine review state. Re-import with same hash + version
    // preserves an existing approval; any change resets to unreviewed.
    const auto rIt = m_review.constFind(s.id);
    if (rIt != m_review.constEnd() && rIt->contentHashSha256 == s.contentHashSha256 &&
        rIt->version == s.version) {
        s.reviewState = rIt->state;
    } else {
        s.reviewState = QStringLiteral("unreviewed");
        // Drop any stale review-state row for this id; the user will
        // re-decide.
        m_review.remove(s.id);
    }

    m_installed.insert(s.id, s);

    if (!writeInstalledManifest())
        return QStringLiteral("could not save the installed-skills list");
    if (!writeReviewManifest())
        return QStringLiteral("could not save the skill review status");

    emit skillsChanged();
    return {};
}

QString SkillService::installFromZip(const QString& zipPath,
                                     const QString& sourceUrl,
                                     const QString& expectedId) {
    if (!m_initialized)
        return QStringLiteral("skills are not ready yet; try again in a moment");

    // Extract into a fresh staging directory.
    const QString stagingRoot = m_skillsRoot + QStringLiteral("/.staging-") +
                                QUuid::createUuid().toString(QUuid::WithoutBraces);
    const auto er = SkillArchive::extract(zipPath, stagingRoot);
    if (!er.success) {
        // Quarantine the failed staging dir so the user can inspect.
        const QString ts = QString::number(QDateTime::currentMSecsSinceEpoch());
        const QString qroot =
            m_quarantineDir + QStringLiteral("/") + ts + QStringLiteral("-zip-extract");
        QDir().rename(stagingRoot, qroot);
        return QStringLiteral("zip extract failed: %1").arg(er.error);
    }

    // Common case: archives wrap the skill folder under one top-level
    // directory. Detect: if staging has exactly one entry and it's a
    // directory, descend into it.
    QDir stagingDir(stagingRoot);
    const auto entries =
        stagingDir.entryInfoList(QDir::Dirs | QDir::Files | QDir::NoDotAndDotDot | QDir::Hidden);
    QString skillFolder = stagingRoot;
    if (entries.size() == 1 && entries.first().isDir()) {
        skillFolder = entries.first().absoluteFilePath();
    }

    // Reuse importSkillFolder by routing through it. importSkillFolder
    // will copy from skillFolder into skills/installed/<id>/, hash,
    // and write manifests. Source defaults to "manual" if no URL given.
    // expectedId is passed as the parser's id-hint so flat-ZIP layouts
    // (no wrapper dir) don't leak the staging directory's `.staging-<uuid>`
    // name through to the installed manifest.
    const QString err = importSkillFolder(skillFolder, expectedId);

    // After importSkillFolder, optionally annotate source.
    if (err.isEmpty() && !sourceUrl.isEmpty()) {
        // Find the just-installed skill and update its source/sourceUrl.
        // installSkillFolder sets source from frontmatter (default
        // "manual"). For ClawHub installs we override to "clawhub".
        for (auto it = m_installed.begin(); it != m_installed.end(); ++it) {
            if (it->installedAtMs == it->updatedAtMs) {
                // Heuristic: most-recent install. Bind URL only if the
                // hash matches (single-shot guard).
                it->source = QStringLiteral("clawhub");
                it->sourceUrl = sourceUrl;
            }
        }
        writeInstalledManifest();
    }

    // Always clean up the staging tree.
    removeFolderRecursive(stagingRoot);

    return err;
}

bool SkillService::approveSkill(const QString& skillId) {
    auto it = m_installed.find(skillId);
    if (it == m_installed.end())
        return false;
    SkillReviewDecision d;
    d.skillId = it->id;
    d.version = it->version;
    d.contentHashSha256 = it->contentHashSha256;
    d.state = QStringLiteral("approved");
    d.decidedAtMs = QDateTime::currentMSecsSinceEpoch();
    d.warningsAtDecisionTime = it->warnings;
    m_review.insert(skillId, d);
    it->reviewState = d.state;
    if (!writeReviewManifest())
        return false;
    if (!writeInstalledManifest())
        return false;
    emit skillsChanged();
    return true;
}

bool SkillService::blockSkill(const QString& skillId) {
    auto it = m_installed.find(skillId);
    if (it == m_installed.end())
        return false;
    SkillReviewDecision d;
    d.skillId = it->id;
    d.version = it->version;
    d.contentHashSha256 = it->contentHashSha256;
    d.state = QStringLiteral("blocked");
    d.decidedAtMs = QDateTime::currentMSecsSinceEpoch();
    d.warningsAtDecisionTime = it->warnings;
    m_review.insert(skillId, d);
    it->reviewState = d.state;
    if (!writeReviewManifest())
        return false;
    if (!writeInstalledManifest())
        return false;
    emit skillsChanged();
    return true;
}

bool SkillService::removeSkill(const QString& skillId) {
    auto it = m_installed.find(skillId);
    if (it == m_installed.end())
        return false;
    const QString abs = m_appDataRoot + QStringLiteral("/") + it->installPath;
    removeFolderRecursive(abs);
    m_installed.erase(it);
    m_review.remove(skillId);
    // Remove from every preferred-list scope.
    for (PreferredSkillsScope& p : m_preferred) {
        p.preferredSkillIds.removeAll(skillId);
    }
    if (!writeInstalledManifest())
        return false;
    if (!writeReviewManifest())
        return false;
    if (!writePreferredManifest())
        return false;
    emit skillsChanged();
    return true;
}

// ---------------------------------------------------------------------------
// Preferred-list APIs
// ---------------------------------------------------------------------------

PreferredSkillsScope* SkillService::findPreferredScope(const QString& type, const QString& id) {
    for (PreferredSkillsScope& p : m_preferred) {
        if (p.scopeType == type && p.scopeId == id)
            return &p;
    }
    return nullptr;
}

const PreferredSkillsScope* SkillService::findPreferredScope(const QString& type,
                                                             const QString& id) const {
    for (const PreferredSkillsScope& p : m_preferred) {
        if (p.scopeType == type && p.scopeId == id)
            return &p;
    }
    return nullptr;
}

PreferredSkillsScope& SkillService::getOrCreatePreferredScope(const QString& type,
                                                              const QString& id) {
    if (auto* existing = findPreferredScope(type, id))
        return *existing;
    PreferredSkillsScope ps;
    ps.scopeType = type;
    ps.scopeId = id;
    m_preferred.append(ps);
    return m_preferred.last();
}

QStringList SkillService::preferredSkillsFor(const QString& scopeType,
                                             const QString& scopeId) const {
    const auto* p = findPreferredScope(scopeType, scopeId);
    if (!p)
        return {};
    return p->preferredSkillIds;
}

bool SkillService::setPreferredSkills(const QString& scopeType,
                                      const QString& scopeId,
                                      const QStringList& skillIds) {
    PreferredSkillsScope& p = getOrCreatePreferredScope(scopeType, scopeId);
    p.preferredSkillIds = skillIds;
    p.updatedAtMs = QDateTime::currentMSecsSinceEpoch();
    if (!writePreferredManifest())
        return false;
    emit preferredSkillsChanged(scopeType, scopeId);
    return true;
}

bool SkillService::exposeOnlyPreferred(const QString& scopeType, const QString& scopeId) const {
    const auto* p = findPreferredScope(scopeType, scopeId);
    return p ? p->exposeOnlyPreferred : false;
}

bool SkillService::setExposeOnlyPreferred(const QString& scopeType,
                                          const QString& scopeId,
                                          bool value) {
    PreferredSkillsScope& p = getOrCreatePreferredScope(scopeType, scopeId);
    p.exposeOnlyPreferred = value;
    p.updatedAtMs = QDateTime::currentMSecsSinceEpoch();
    if (!writePreferredManifest())
        return false;
    emit preferredSkillsChanged(scopeType, scopeId);
    return true;
}

bool SkillService::overrideParentFolder(const QString& conversationId) const {
    // Override flag lives on whichever conversation_* scope row matches
    // the conv id. We don't know the type without consulting the
    // conversation service, so try both common types.
    for (const auto* type : {"conversation_1to1", "conversation_group"}) {
        const auto* p = findPreferredScope(QString::fromLatin1(type), conversationId);
        if (p)
            return p->overrideParentFolder;
    }
    return false;
}

bool SkillService::setOverrideParentFolder(const QString& conversationId, bool value) {
    // Same dual-type approach: read the conversation's actual type from
    // ConversationService so we write the right scope row.
    const auto co = m_convs.getConversation(conversationId);
    if (!co.has_value())
        return false;
    const QString scopeType =
        co->isGroup ? QStringLiteral("conversation_group") : QStringLiteral("conversation_1to1");
    PreferredSkillsScope& p = getOrCreatePreferredScope(scopeType, conversationId);
    p.overrideParentFolder = value;
    p.updatedAtMs = QDateTime::currentMSecsSinceEpoch();
    if (!writePreferredManifest())
        return false;
    emit preferredSkillsChanged(scopeType, conversationId);
    return true;
}

// ---------------------------------------------------------------------------
// Resolver
// ---------------------------------------------------------------------------

SkillService::ResolvedScope SkillService::resolveForConversation(const QString& conversationId) {
    ResolvedScope out;
    if (conversationId.isEmpty())
        return out;

    const auto co = m_convs.getConversation(conversationId);
    if (!co.has_value())
        return out;
    const QString convScopeType =
        co->isGroup ? QStringLiteral("conversation_group") : QStringLiteral("conversation_1to1");

    const PreferredSkillsScope* convPref = findPreferredScope(convScopeType, conversationId);
    const PreferredSkillsScope* folderPref = nullptr;
    if (!co->folderId.isEmpty()) {
        folderPref = findPreferredScope(QStringLiteral("folder"), co->folderId);
    }

    auto filterApproved = [this](const QStringList& ids) -> QStringList {
        QStringList out;
        for (const QString& id : ids) {
            const auto it = m_installed.constFind(id);
            if (it == m_installed.constEnd())
                continue;
            if (!it->isApproved())
                continue;
            out.append(id);
        }
        return out;
    };

    auto useScope = [&](const PreferredSkillsScope& src) {
        out.preferredSkillIds = filterApproved(src.preferredSkillIds);
        out.exposeOnly = src.exposeOnlyPreferred;
    };

    if (convPref && convPref->overrideParentFolder && !convPref->preferredSkillIds.isEmpty()) {
        useScope(*convPref);
    } else if (folderPref && !folderPref->preferredSkillIds.isEmpty()) {
        useScope(*folderPref);
    } else if (convPref && !convPref->preferredSkillIds.isEmpty()) {
        useScope(*convPref);
    }
    return out;
}

SkillService::ResolvedScope SkillService::resolveForFolder(const QString& folderId) const {
    ResolvedScope out;
    if (folderId.isEmpty())
        return out;
    const auto* p = findPreferredScope(QStringLiteral("folder"), folderId);
    if (!p)
        return out;
    QStringList filtered;
    for (const QString& id : p->preferredSkillIds) {
        const auto it = m_installed.constFind(id);
        if (it == m_installed.constEnd())
            continue;
        if (!it->isApproved())
            continue;
        filtered.append(id);
    }
    out.preferredSkillIds = filtered;
    out.exposeOnly = p->exposeOnlyPreferred;
    return out;
}

// ---------------------------------------------------------------------------
// Filesystem helpers
// ---------------------------------------------------------------------------

bool SkillService::removeFolderRecursive(const QString& absPath) {
    QDir d(absPath);
    if (!d.exists())
        return true;
    return d.removeRecursively();
}

bool SkillService::copyFolderRecursive(const QString& src, const QString& dst, QString* outError) {
    QDir srcDir(src);
    if (!srcDir.exists()) {
        if (outError)
            *outError = QStringLiteral("source missing: %1").arg(src);
        return false;
    }
    if (!QDir().mkpath(dst)) {
        if (outError)
            *outError = QStringLiteral("cannot create %1").arg(dst);
        return false;
    }
    const auto entries =
        srcDir.entryInfoList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden);
    for (const QFileInfo& fi : entries) {
        const QString dstChild = dst + QStringLiteral("/") + fi.fileName();
        if (fi.isSymLink()) {
            if (outError)
                *outError = QStringLiteral("symbolic links are not allowed in skills: %1")
                                .arg(fi.fileName());
            return false;
        }
        if (fi.isDir()) {
            if (!copyFolderRecursive(fi.absoluteFilePath(), dstChild, outError))
                return false;
        } else if (fi.isFile()) {
            if (!QFile::copy(fi.absoluteFilePath(), dstChild)) {
                if (outError)
                    *outError = QStringLiteral("copy failed: %1 → %2").arg(fi.fileName(), dstChild);
                return false;
            }
        }
    }
    return true;
}
