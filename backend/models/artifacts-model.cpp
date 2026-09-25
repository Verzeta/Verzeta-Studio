// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file artifacts-model.cpp
 * @brief Push-based artifacts list. Each row is derived from a persisted
 *        message: tool rows that wrote a file, or assistant rows with
 *        finishReason="artifact" from the plan submit_result pipeline.
 * @layer Service (Model)
 * @dependencies MessageService, Qt6::Core.
 */

#include "artifacts-model.h"

#include "../models/message.h"
#include "../services/message-service.h"
#include "../utils/logger.h"

#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>

ArtifactsModel::ArtifactsModel(MessageService& svc, QObject* parent)
    : QAbstractListModel(parent), m_svc(svc) {
    connect(&m_svc, &MessageService::messageAdded, this, &ArtifactsModel::onMessageAdded);
    connect(&m_svc, &MessageService::messageDeleted, this, &ArtifactsModel::onMessageDeleted);
}

void ArtifactsModel::setActiveConversation(const QString& convId) {
    if (convId == m_activeConvId)
        return;
    m_activeConvId = convId;
    scheduleReload();  // deferred so the chat view opens first
    emit activeConversationChanged();
}

void ArtifactsModel::scheduleReload() {
    if (m_reloadScheduled)
        return;  // coalesce rapid switches into one reload
    m_reloadScheduled = true;
    // Defer to the next event-loop cycle: the chat view's own message load must
    // paint first; this secondary model then reloads for whatever conversation
    // is active by then (rapid A->B->A collapses to a single reload for A).
    QMetaObject::invokeMethod(
        this,
        [this]() {
            m_reloadScheduled = false;
            reload();
        },
        Qt::QueuedConnection);
}

QString ArtifactsModel::activeConversationId() const {
    return m_activeConvId;
}

void ArtifactsModel::setWorkspaceDir(const QString& dir) {
    // Store only; AppController calls this immediately before
    // setActiveConversation(), whose reload() picks up the new value. This
    // keeps a conversation switch to a single reload.
    m_workspaceDir = dir;
}

int ArtifactsModel::count() const {
    return static_cast<int>(m_rows.size());
}

int ArtifactsModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return static_cast<int>(m_rows.size());
}

QVariant ArtifactsModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(m_rows.size())) {
        return {};
    }
    const QVariantMap& f = m_rows.at(index.row()).fields;
    switch (role) {
        case PathRole:
            return f.value(QStringLiteral("path"));
        case FileNameRole:
            return f.value(QStringLiteral("fileName"));
        case ToolNameRole:
            return f.value(QStringLiteral("toolName"));
        case PlanIdRole:
            return f.value(QStringLiteral("planId"));
        case StepIdRole:
            return f.value(QStringLiteral("stepId"));
        case StepTitleRole:
            return f.value(QStringLiteral("stepTitle"));
        case PlanGoalRole:
            return f.value(QStringLiteral("planGoal"));
        case SubmittedByRole:
            return f.value(QStringLiteral("submittedBy"));
        default:
            return {};
    }
}

QHash<int, QByteArray> ArtifactsModel::roleNames() const {
    return {
        {PathRole, QByteArrayLiteral("path")},
        {FileNameRole, QByteArrayLiteral("fileName")},
        {ToolNameRole, QByteArrayLiteral("toolName")},
        {PlanIdRole, QByteArrayLiteral("planId")},
        {StepIdRole, QByteArrayLiteral("stepId")},
        {StepTitleRole, QByteArrayLiteral("stepTitle")},
        {PlanGoalRole, QByteArrayLiteral("planGoal")},
        {SubmittedByRole, QByteArrayLiteral("submittedBy")},
    };
}

void ArtifactsModel::onMessageAdded(const QString& convId, const QString& msgId) {
    if (convId != m_activeConvId)
        return;
    const QList<Message> all = m_svc.getMessages(convId);
    for (const Message& m : all) {
        if (m.id != msgId)
            continue;
        const QList<Row> produced = extractFromMessage(m);
        if (produced.isEmpty())
            return;

        // Skip a produced row whose file is already listed (e.g. a file
        // re-written in the same conversation, or already present via the
        // workspace scan) so the live insert cannot introduce a duplicate.
        QSet<QString> existing;
        for (const Row& r : m_rows) {
            const QString k = pathKey(r.fields.value(QStringLiteral("path")).toString());
            if (!k.isEmpty())
                existing.insert(k);
        }
        QList<Row> fresh;
        for (const Row& r : produced) {
            const QString k = pathKey(r.fields.value(QStringLiteral("path")).toString());
            if (!k.isEmpty()) {
                if (existing.contains(k))
                    continue;
                existing.insert(k);
            }
            fresh.append(r);
        }
        if (fresh.isEmpty())
            return;

        const int firstRow = static_cast<int>(m_rows.size());
        const int lastRow = firstRow + fresh.size() - 1;
        beginInsertRows({}, firstRow, lastRow);
        for (const Row& r : fresh)
            m_rows.append(r);
        endInsertRows();
        emit countChanged();
        return;
    }
}

void ArtifactsModel::onMessageDeleted(const QString& convId, const QString& msgId) {
    if (convId != m_activeConvId)
        return;
    // Walk and remove ALL rows sourced from this message (may be >1).
    for (int i = static_cast<int>(m_rows.size()) - 1; i >= 0; --i) {
        if (m_rows.at(i).sourceMsgId != msgId)
            continue;
        beginRemoveRows({}, i, i);
        m_rows.removeAt(i);
        endRemoveRows();
    }
    emit countChanged();
}

int ArtifactsModel::findRowByMsgId(const QString& msgId) const {
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows.at(i).sourceMsgId == msgId)
            return i;
    }
    return -1;
}

QString ArtifactsModel::pathKey(const QString& path) const {
    if (path.isEmpty())
        return {};
    QString abs;
    const QFileInfo fi(path);
    if (fi.isAbsolute())
        abs = path;
    else if (!m_workspaceDir.isEmpty())
        abs = m_workspaceDir + QLatin1Char('/') + path;
    else
        abs = fi.absoluteFilePath();
    // Canonicalise when the file exists (resolves symlinks, "." and "..");
    // fall back to a cleaned absolute path when it does not.
    const QString canon = QFileInfo(abs).canonicalFilePath();
    return canon.isEmpty() ? QDir::cleanPath(abs) : canon;
}

void ArtifactsModel::reload() {
    beginResetModel();
    m_rows.clear();

    // Every file is listed once. Rows come from two sources — the
    // conversation's own messages (rich metadata: tool, plan, submitter)
    // and the shared workspace directory — deduplicated by a normalised
    // path key so a file that a message produced (relative path) is not
    // duplicated by the workspace scan (absolute path), nor listed twice
    // when it was written more than once.
    QSet<QString> seen;

    // 1) Message-produced artifacts, first, so they win the metadata.
    if (!m_activeConvId.isEmpty()) {
        const QList<Message> all = m_svc.getMessages(m_activeConvId);
        for (const Message& m : all) {
            for (const Row& r : extractFromMessage(m)) {
                const QString key = pathKey(r.fields.value(QStringLiteral("path")).toString());
                // Plan artifacts (Case B) have no path -> empty key -> always
                // kept (each is a distinct submitted-result record).
                if (!key.isEmpty()) {
                    if (seen.contains(key))
                        continue;
                    seen.insert(key);
                }
                m_rows.append(r);
            }
        }
    }

    // 2) Every other file in the shared workspace directory. This is why a
    //    1:1 chat inside a project shows the same files as the group chat:
    //    they share the project workspace. Plain rows (absolute path +
    //    fileName), capped to keep a huge workspace from flooding the list.
    if (!m_workspaceDir.isEmpty()) {
        constexpr int kMaxWorkspaceFiles = 2000;
        constexpr int kMaxWorkspaceDirs = 4000;

        // Dependency / build / VCS directories are NOT artifacts and can hold
        // tens of thousands of files (e.g. node_modules after `npm install`).
        // A recursive QDirIterator would traverse them all on the UI thread and
        // freeze chat-open for seconds, so we walk manually and PRUNE these
        // (and any hidden directory) instead of descending into them.
        static const QSet<QString> kSkipDirs = {
            QStringLiteral("node_modules"), QStringLiteral(".git"),
            QStringLiteral(".svn"),         QStringLiteral(".hg"),
            QStringLiteral(".venv"),        QStringLiteral("venv"),
            QStringLiteral("env"),          QStringLiteral("__pycache__"),
            QStringLiteral(".mypy_cache"),  QStringLiteral(".pytest_cache"),
            QStringLiteral("dist"),         QStringLiteral("build"),
            QStringLiteral(".next"),        QStringLiteral(".nuxt"),
            QStringLiteral("target"),       QStringLiteral(".cache"),
            QStringLiteral("vendor"),       QStringLiteral(".gradle"),
            QStringLiteral(".idea"),        QStringLiteral(".vscode"),
            QStringLiteral("coverage"),     QStringLiteral(".tox"),
            QStringLiteral(".verzeta")  // our own per-process log dir
        };

        int added = 0;
        int dirsVisited = 0;
        bool capped = false;
        QStringList dirQueue{m_workspaceDir};
        while (!dirQueue.isEmpty() && added < kMaxWorkspaceFiles &&
               dirsVisited < kMaxWorkspaceDirs) {
            const QString dir = dirQueue.takeFirst();
            ++dirsVisited;
            QDirIterator it(dir,
                            QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable,
                            QDirIterator::NoIteratorFlags);  // one level only
            while (it.hasNext()) {
                const QFileInfo fi(it.next());
                const QString base = fi.fileName();
                if (fi.isDir()) {
                    // Prune hidden + heavy dirs; queue the rest for the walk.
                    if (base.startsWith(QLatin1Char('.')) || kSkipDirs.contains(base))
                        continue;
                    dirQueue.append(fi.absoluteFilePath());
                    continue;
                }
                if (base.startsWith(QLatin1Char('.')))
                    continue;  // hidden file
                if (added >= kMaxWorkspaceFiles) {
                    capped = true;
                    break;
                }
                const QString abs = fi.absoluteFilePath();
                const QString key = pathKey(abs);
                if (seen.contains(key))
                    continue;  // already listed
                seen.insert(key);
                Row r;
                r.fields[QStringLiteral("path")] = abs;
                r.fields[QStringLiteral("fileName")] = base;
                m_rows.append(r);
                ++added;
            }
        }
        if (capped || !dirQueue.isEmpty() || dirsVisited >= kMaxWorkspaceDirs) {
            qCInfo(verzetaUi) << "ArtifactsModel: workspace scan capped at" << kMaxWorkspaceFiles
                              << "files /" << kMaxWorkspaceDirs << "dirs for" << m_workspaceDir;
        }
    }

    endResetModel();
    emit countChanged();
}

QList<ArtifactsModel::Row> ArtifactsModel::extractFromMessage(const Message& m) {
    QList<Row> out;

    // Case A: role="tool" — extract "path" from the JSON result.
    if (m.role == QStringLiteral("tool") && !m.content.isEmpty()) {
        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(m.content.toUtf8(), &err);
        if (err.error == QJsonParseError::NoError) {
            const QJsonObject obj = doc.object();
            const QString path = obj[QStringLiteral("path")].toString();
            if (!path.isEmpty()) {
                Row r;
                r.sourceMsgId = m.id;
                r.fields[QStringLiteral("path")] = path;
                r.fields[QStringLiteral("fileName")] = QFileInfo(path).fileName();
                r.fields[QStringLiteral("toolName")] =
                    m.metadata[QStringLiteral("tool_name")].toString();
                out.append(r);
            }
        }
        return out;
    }

    // Case C: generated image filed into the project workspace. The
    // image pipeline is asynchronous — the generate_image TOOL result
    // carries no path (status "queued"), and the completion lands
    // later as a plain assistant message from ImageService — so
    // neither Case A nor Case B can ever match one. The completion
    // message's workspace_path metadata (set when the PNG is filed
    // into <project>/images/) is the artifact record.
    if (m.role == QStringLiteral("assistant") &&
        m.metadata[QStringLiteral("produced_by")].toString() == QStringLiteral("image_service")) {
        const QString wsPath = m.metadata[QStringLiteral("workspace_path")].toString();
        if (!wsPath.isEmpty()) {
            Row r;
            r.sourceMsgId = m.id;
            r.fields[QStringLiteral("path")] = wsPath;
            r.fields[QStringLiteral("fileName")] = QFileInfo(wsPath).fileName();
            r.fields[QStringLiteral("toolName")] = QStringLiteral("generate_image");
            out.append(r);
        }
        return out;
    }

    // Case B: role="assistant" with finishReason="artifact" — plan artifact.
    if (m.role == QStringLiteral("assistant") && m.finishReason == QStringLiteral("artifact")) {
        Row r;
        r.sourceMsgId = m.id;
        r.fields[QStringLiteral("path")] = QString();
        r.fields[QStringLiteral("fileName")] = m.metadata[QStringLiteral("summary")].toString();
        r.fields[QStringLiteral("toolName")] = QStringLiteral("submit_result");
        r.fields[QStringLiteral("planId")] = m.metadata[QStringLiteral("plan_id")].toString();
        r.fields[QStringLiteral("stepId")] = m.metadata[QStringLiteral("step_id")].toString();
        r.fields[QStringLiteral("stepTitle")] = m.metadata[QStringLiteral("step_title")].toString();
        r.fields[QStringLiteral("planGoal")] = m.metadata[QStringLiteral("plan_goal")].toString();
        r.fields[QStringLiteral("submittedBy")] = m.memberAlias;
        out.append(r);
    }

    return out;
}
