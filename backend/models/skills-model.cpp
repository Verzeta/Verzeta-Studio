// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file skills-model.cpp
 * @brief Implementation of SkillsModel, which subscribes to SkillService
 *        skillsChanged and rebuilds the row list on each change.
 * @layer Service (Model)
 * @dependencies SkillService, Qt6::Core.
 */

#include "skills-model.h"

#include "../services/skill-service.h"

#include <QVariantList>

SkillsModel::SkillsModel(SkillService& svc, QObject* parent)
    : QAbstractListModel(parent), m_svc(svc) {
    connect(&m_svc, &SkillService::skillsChanged, this, &SkillsModel::rebuild);
    rebuild();
}

SkillsModel::~SkillsModel() = default;

int SkillsModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return m_rows.size();
}

QVariant SkillsModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid())
        return {};
    const int row = index.row();
    if (row < 0 || row >= m_rows.size())
        return {};
    const Skill& s = m_rows[row];
    switch (role) {
        case IdRole:
            return s.id;
        case DescriptionRole:
            return s.description;
        case SourceRole:
            return s.source;
        case VersionRole:
            return s.version;
        case TagsRole:
            return s.tags;
        case DeclaredToolsRole:
            return s.declaredTools;
        case ContentHashShortRole:
            return s.contentHashSha256.left(12);
        case InstalledAtMsRole:
            return s.installedAtMs;
        case ReviewStateRole:
            return s.reviewState;
        case WarningCountRole:
            return s.warnings.size();
        default:
            return {};
    }
}

QHash<int, QByteArray> SkillsModel::roleNames() const {
    return {
        {IdRole, "skillId"},
        {DescriptionRole, "description"},
        {SourceRole, "source"},
        {VersionRole, "version"},
        {TagsRole, "tags"},
        {DeclaredToolsRole, "declaredTools"},
        {ContentHashShortRole, "contentHashShort"},
        {InstalledAtMsRole, "installedAtMs"},
        {ReviewStateRole, "reviewState"},
        {WarningCountRole, "warningCount"},
    };
}

void SkillsModel::rebuild() {
    beginResetModel();
    m_rows.clear();
    // SkillService doesn't expose the underlying QHash directly; we
    // rebuild from installedSkills() (list of QVariantMap). For S1
    // this is fine — the variant-map round-trip is cheap because the
    // installed library stays small in practice.
    const QVariantList list = m_svc.installedSkills();
    m_rows.reserve(list.size());
    for (const QVariant& v : list) {
        const QVariantMap m = v.toMap();
        Skill s;
        s.id = m.value(QStringLiteral("id")).toString();
        s.source = m.value(QStringLiteral("source")).toString();
        s.version = m.value(QStringLiteral("version")).toString();
        s.description = m.value(QStringLiteral("description")).toString();
        s.tags = m.value(QStringLiteral("tags")).toStringList();
        s.declaredTools = m.value(QStringLiteral("declaredTools")).toStringList();
        s.contentHashSha256 = m.value(QStringLiteral("contentHashSha256")).toString();
        s.installedAtMs = m.value(QStringLiteral("installedAtMs")).toLongLong();
        s.reviewState = m.value(QStringLiteral("reviewState")).toString();
        const QVariantList wlist = m.value(QStringLiteral("warnings")).toList();
        for (const QVariant& wv : wlist) {
            const QVariantMap wm = wv.toMap();
            SkillWarning w;
            w.regexName = wm.value(QStringLiteral("regexName")).toString();
            w.fileRelativePath = wm.value(QStringLiteral("fileRelativePath")).toString();
            w.lineNumber = wm.value(QStringLiteral("lineNumber")).toInt();
            w.matchedExcerpt = wm.value(QStringLiteral("matchedExcerpt")).toString();
            s.warnings.append(w);
        }
        m_rows.append(s);
    }
    endResetModel();
}
