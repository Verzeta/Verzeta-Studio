// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file skills-model.h
 * @brief QAbstractListModel exposing the installed skills library to QML.
 *
 *        Push-based: subscribes to SkillService::skillsChanged in its
 *        constructor and reloads the row list on every change. The
 *        library stays small enough that a full reset is cheaper than
 *        a per-row diff.
 * @layer Model (Frontend-facing)
 * @dependencies Qt6::Core, SkillService.
 */


#pragma once

#include "skill.h"

#include <QAbstractListModel>
#include <QHash>
#include <QString>
#include <QStringList>

class SkillService;

/**
 * @brief QAbstractListModel exposing one row per installed skill,
 *        kept in sync with SkillService via skillsChanged.
 */
class SkillsModel : public QAbstractListModel {
    Q_OBJECT

  public:
    /** @brief Item data roles exposed to QML delegates. */
    enum Roles {
        IdRole = Qt::UserRole + 1,
        DescriptionRole,
        SourceRole,
        VersionRole,
        TagsRole,
        DeclaredToolsRole,
        ContentHashShortRole,
        InstalledAtMsRole,
        ReviewStateRole,
        WarningCountRole,
    };

    /**
     * @brief Constructs the model bound to a SkillService.
     * @param svc     SkillService whose skillsChanged drives rebuilds.
     *                Must outlive this object.
     * @param parent  Optional Qt parent.
     */
    explicit SkillsModel(SkillService& svc, QObject* parent = nullptr);
    ~SkillsModel() override;

    /**
     * @brief QAbstractListModel row count.
     * @param parent  Ignored (flat list).
     * @returns Row count.
     */
    int rowCount(const QModelIndex& parent = {}) const override;

    /**
     * @brief QAbstractListModel data accessor.
     * @param index  Row index requested.
     * @param role   Role from the Roles enum (Qt::DisplayRole maps to
     *               the description for view convenience).
     * @returns QVariant with the role value.
     */
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;

    /**
     * @brief QAbstractListModel role-name map for QML.
     * @returns Hash mapping Roles enum values to QML role names.
     */
    QHash<int, QByteArray> roleNames() const override;

  private slots:
    /**
     * @brief Rebuilds the row list from SkillService's current snapshot.
     */
    void rebuild();

  private:
    SkillService& m_svc;
    QList<Skill> m_rows;
};
