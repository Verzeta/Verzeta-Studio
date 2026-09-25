// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file artifacts-model.h
 * @brief Push-based QAbstractListModel of file artifacts for the
 *        active conversation. Rows represent files produced by tool calls
 *        (extracted from the `path` field of role="tool" message result
 *        JSON) plus plan-artifact messages (finishReason="artifact"). All
 *        updates flow from MessageService signals, with no polling and no pull.
 * @layer Service (Model)
 * @dependencies MessageService, Qt6::Core
 */

#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QObject>
#include <QString>
#include <QVariantMap>

class MessageService;

/**
 * @brief QAbstractListModel exposing one row per file artifact in the
 *        active conversation.
 *
 * Rows are derived from `role="tool"` messages whose result JSON includes
 * a `path` field, plus `role="assistant"` messages with
 * `finishReason="artifact"` produced by the plan submit_result pipeline.
 * Push-driven from MessageService signals; QML never polls.
 */
class ArtifactsModel : public QAbstractListModel {
    Q_OBJECT
    Q_PROPERTY(int count READ count NOTIFY countChanged)
    Q_PROPERTY(QString activeConversationId READ activeConversationId WRITE setActiveConversation
                   NOTIFY activeConversationChanged)

  public:
    /** @brief Item data roles exposed to QML delegates. */
    enum Roles {
        PathRole = Qt::UserRole + 1,
        FileNameRole,
        ToolNameRole,
        PlanIdRole,
        StepIdRole,
        StepTitleRole,
        PlanGoalRole,
        SubmittedByRole
    };
    Q_ENUM(Roles)

    /**
     * @brief Constructs the model bound to a MessageService.
     * @param svc     MessageService whose signals drive the model.
     *                Must outlive this object.
     * @param parent  Optional Qt parent.
     */
    explicit ArtifactsModel(MessageService& svc, QObject* parent = nullptr);

    /**
     * @brief Loads artifact rows for the given conversation.
     * @param convId  Conversation UUID. Empty clears the model.
     */
    void setActiveConversation(const QString& convId);

    /**
     * @brief Sets the shared workspace directory whose files are listed
     *        alongside the message-produced artifacts.
     *
     * For a project/organization conversation this is the project workspace,
     * so every chat in the project (group OR 1:1) sees the same files, not
     * just the ones its own messages produced. Empty disables the scan.
     * Stores only; the reload happens on the following setActiveConversation.
     *
     * @param dir  Absolute workspace directory path, or empty to disable.
     */
    void setWorkspaceDir(const QString& dir);

    /**
     * @brief Returns the currently-bound conversation id.
     * @returns Conversation UUID, or empty when no scope is set.
     */
    QString activeConversationId() const;

    /**
     * @brief Returns the number of artifact rows in the model.
     * @returns Row count.
     */
    int count() const;

    /**
     * @brief QAbstractListModel row count.
     * @param parent  Ignored (flat list).
     * @returns Row count.
     */
    int rowCount(const QModelIndex& parent = {}) const override;

    /**
     * @brief QAbstractListModel data accessor.
     * @param index  Row index requested.
     * @param role   Role from the Roles enum.
     * @returns QVariant with the role value, or an invalid QVariant when
     *          index / role is out of range.
     */
    QVariant data(const QModelIndex& index, int role) const override;

    /**
     * @brief QAbstractListModel role-name map for QML.
     * @returns Hash mapping Roles enum values to QML role names.
     */
    QHash<int, QByteArray> roleNames() const override;

  signals:
    /** @brief Emitted whenever the row count changes. */
    void countChanged();

    /** @brief Emitted whenever setActiveConversation changes the bound conv. */
    void activeConversationChanged();

  private slots:
    /**
     * @brief Inserts a row when a message in the active scope yields a
     *        new artifact.
     * @param convId  Conversation id of the inserted message.
     * @param msgId   Inserted message id.
     */
    void onMessageAdded(const QString& convId, const QString& msgId);

    /**
     * @brief Removes any rows derived from a deleted message.
     * @param convId  Conversation id of the deleted message.
     * @param msgId   Deleted message id.
     */
    void onMessageDeleted(const QString& convId, const QString& msgId);

  private:
    MessageService& m_svc;
    QString m_activeConvId;
    QString m_workspaceDir;  // shared project workspace, scanned in reload()

    /**
     * @brief One row in the model. We store a cooked QVariantMap per
     *        artifact since the source messages are heterogeneous
     *        (tool rows vs artifact-finishReason assistant rows).
     */
    struct Row {
        QString sourceMsgId;
        QVariantMap fields;
    };
    QList<Row> m_rows;

    int findRowByMsgId(const QString& msgId) const;
    void reload();

    /**
     * @brief Coalesced, deferred reload. A conversation switch schedules the
     *        reload on the next event-loop cycle instead of running it inline,
     *        so the chat view opens first and this (secondary) artifacts model
     *        does not add a redundant full-message load to the chat-open path.
     *        Rapid switches collapse into a single reload for the latest
     *        conversation.
     */
    void scheduleReload();

    bool m_reloadScheduled = false;  ///< true while a deferred reload is queued

    /**
     * @brief Normalises a file path to a stable dedup key.
     *
     * Tool results store paths RELATIVE to the workspace (e.g. "report.md"),
     * while the workspace scan yields absolute paths. Resolving relative
     * paths against m_workspaceDir (and canonicalising when the file exists)
     * lets both sources map to the same key so a file is listed once.
     *
     * @param path  A row's path (relative or absolute); empty returns empty.
     * @returns Canonical/absolute key, or empty for an empty input.
     */
    QString pathKey(const QString& path) const;

    /**
     * @brief Extracts the artifact row(s) a single Message yields.
     *        Returns zero or more rows: a tool message with no file path
     *        produces no row, a file-producing tool message produces one,
     *        and a plan-artifact assistant message produces one too.
     */
    static QList<Row> extractFromMessage(const class Message& m);
};
