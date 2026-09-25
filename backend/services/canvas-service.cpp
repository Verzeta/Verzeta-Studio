// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file canvas-service.cpp
 * @brief Implementation of CanvasService.
 *
 *        The canvas schema lives in DbManager. Write paths persist
 *        through DbManager + mirror to disk via FileService and emit
 *        the matching signals.
 * @layer Service
 * @dependencies Qt6::Sql via DbManager, models/conversation.h
 *               (forward, only via ConversationService),
 *               services/file-service.h (forward, only via setter),
 *               utils/logger.h, utils/thread-discipline.h.
 */

#include "canvas-service.h"

#include "../models/conversation.h"
#include "../models/db-manager.h"
#include "../services/conversation-service.h"
#include "../services/file-service.h"
#include "../utils/logger.h"
#include "../utils/thread-discipline.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>
#include <QSet>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QUuid>
#include <QVariant>

namespace {

QVariantMap canvasRowToMap(QSqlQuery& q) {
    QVariantMap m;
    const QString content = q.value(QStringLiteral("content")).toString();
    m.insert(QStringLiteral("id"), q.value(QStringLiteral("id")).toString());
    m.insert(QStringLiteral("conversationId"),
             q.value(QStringLiteral("conversation_id")).toString());
    m.insert(QStringLiteral("filename"), q.value(QStringLiteral("filename")).toString());
    m.insert(QStringLiteral("language"), q.value(QStringLiteral("language")).toString());
    m.insert(QStringLiteral("content"), content);
    m.insert(QStringLiteral("revision"), q.value(QStringLiteral("revision")).toInt());
    m.insert(QStringLiteral("isArchived"), q.value(QStringLiteral("is_archived")).toInt() != 0);
    m.insert(QStringLiteral("sourceMsgId"), q.value(QStringLiteral("source_msg_id")).toString());
    m.insert(QStringLiteral("createdAt"), q.value(QStringLiteral("created_at")).toString());
    m.insert(QStringLiteral("updatedAt"), q.value(QStringLiteral("updated_at")).toString());
    // Convenience fields the prompt-metadata layer + UI consume.
    m.insert(QStringLiteral("byteSize"), content.size());
    m.insert(QStringLiteral("lineCount"),
             content.isEmpty() ? 0 : (content.count(QLatin1Char('\n')) + 1));
    return m;
}

}  // anonymous namespace

CanvasService::CanvasService(DbManager& db, QObject* parent) : QObject(parent), m_db(db) {
    qCInfo(verzetaUi) << "CanvasService initialized";
}

CanvasService::~CanvasService() {
    qCInfo(verzetaUi) << "CanvasService destroyed";
}

void CanvasService::setFileService(FileService* svc) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (m_fileSvc == svc)
        return;
    if (m_fileSvc) {
        // Disconnect any prior subscription before re-attaching to
        // avoid duplicate auto-promote firings.
        disconnect(m_fileSvc, &FileService::fileSaved, this, nullptr);
    }
    m_fileSvc = svc;
    if (m_fileSvc) {
        connect(m_fileSvc, &FileService::fileSaved, this, [this](const QString& filePath) {
            // Re-entry guard — our own writeDiskMirror fires
            // FileService::fileSaved through saveGeneratedFile.
            // Without this guard the auto-promote would
            // recurse infinitely.
            if (m_inWriteMirror)
                return;
            if (!m_activeConvIdGetter)
                return;
            const QString convId = m_activeConvIdGetter();
            if (convId.isEmpty())
                return;
            QFile f(filePath);
            if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
                qCWarning(verzetaUi) << "CanvasService auto-promote: cannot read" << filePath << "—"
                                     << f.errorString();
                return;
            }
            const QByteArray bytes = f.readAll();
            f.close();
            const QString content = QString::fromUtf8(bytes);
            const QString filename = QFileInfo(filePath).fileName();
            tryAutoPromote(convId, filename, content);
        });
    }
    qCInfo(verzetaUi) << "CanvasService: FileService" << (svc ? "attached" : "detached");
}

void CanvasService::setConversationService(ConversationService* svc) {
    VERZETA_ASSERT_MAIN_THREAD();
    m_convSvc = svc;
    qCInfo(verzetaUi) << "CanvasService: ConversationService" << (svc ? "attached" : "detached");
}

void CanvasService::setActiveConversationIdGetter(std::function<QString()> getter) {
    VERZETA_ASSERT_MAIN_THREAD();
    m_activeConvIdGetter = std::move(getter);
}


QVariantMap CanvasService::activeCanvasFor(const QString& conversationId) const {
    VERZETA_ASSERT_MAIN_THREAD();
    if (conversationId.isEmpty())
        return {};

    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT id, conversation_id, filename, language, content, "
                             "       revision, is_archived, source_msg_id, created_at, updated_at "
                             "FROM canvas_artifacts "
                             "WHERE conversation_id = ? AND is_archived = 0 "
                             "ORDER BY updated_at DESC "
                             "LIMIT 1"));
    q.addBindValue(conversationId);
    if (!q.exec()) {
        qCWarning(verzetaUi) << "CanvasService::activeCanvasFor query failed:"
                             << q.lastError().text();
        return {};
    }
    if (!q.next())
        return {};
    return canvasRowToMap(q);
}

QVariantList CanvasService::historyForConversation(const QString& conversationId) const {
    VERZETA_ASSERT_MAIN_THREAD();
    if (conversationId.isEmpty())
        return {};

    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT id, conversation_id, filename, language, content, "
                             "       revision, is_archived, source_msg_id, created_at, updated_at "
                             "FROM canvas_artifacts "
                             "WHERE conversation_id = ? "
                             "ORDER BY updated_at DESC"));
    q.addBindValue(conversationId);
    if (!q.exec()) {
        qCWarning(verzetaUi) << "CanvasService::historyForConversation query failed:"
                             << q.lastError().text();
        return {};
    }

    QVariantList out;
    while (q.next()) {
        out.append(canvasRowToMap(q));
    }
    return out;
}


QString CanvasService::openCanvas(const QString& conversationId,
                                  const QString& filename,
                                  const QString& language,
                                  const QString& content,
                                  const QString& sourceMsgId) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (conversationId.isEmpty() || filename.isEmpty() || language.isEmpty()) {
        qCWarning(verzetaUi) << "CanvasService::openCanvas: required arg missing —"
                             << "convId:" << conversationId << "filename:" << filename
                             << "language:" << language;
        return {};
    }

    const QString nowIso = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);

    QString existingId;
    int existingRevision = 0;
    bool existingArchived = false;
    {
        QSqlQuery sel(m_db.db());
        sel.prepare(QStringLiteral("SELECT id, revision, is_archived "
                                   "FROM canvas_artifacts "
                                   "WHERE conversation_id = ? AND filename = ? "
                                   "ORDER BY updated_at DESC LIMIT 1"));
        sel.addBindValue(conversationId);
        sel.addBindValue(filename);
        if (!sel.exec()) {
            qCWarning(verzetaUi) << "CanvasService::openCanvas: filename lookup failed:"
                                 << sel.lastError().text();
            return {};
        }
        if (sel.next()) {
            existingId = sel.value(0).toString();
            existingRevision = sel.value(1).toInt();
            existingArchived = sel.value(2).toInt() != 0;
        }
    }

    // Archive-then-write must be ATOMIC. If the insert/upsert below fails
    // AFTER the archive has committed, the conversation is left with ZERO
    // active canvases (every row archived, no new active row) — which
    // stranded a conversation whenever an open failed. Wrap both writes in
    // one transaction; only manage it when we actually started one (a
    // caller may already hold an outer transaction).
    QSqlDatabase txnDb = m_db.db();
    const bool startedTxn = txnDb.transaction();

    // Archive any current active canvas (different filename) so the
    // "at most one active canvas per conversation" invariant holds.
    // If the existing row IS the current active row, no archive is
    // needed — the upsert below just bumps its revision.
    //
    // SQL note: "WHERE id != ?" with `?` bound to an empty/NULL value
    // evaluates to NULL (never TRUE) in three-valued logic, which
    // silently archives NOTHING. We split into two branches to keep
    // the archive predicate unambiguous when there is no existing
    // match for this filename.
    {
        QSqlQuery arch(m_db.db());
        if (existingId.isEmpty()) {
            arch.prepare(QStringLiteral("UPDATE canvas_artifacts "
                                        "SET is_archived = 1, updated_at = ? "
                                        "WHERE conversation_id = ? "
                                        "  AND is_archived    = 0"));
            arch.addBindValue(nowIso);
            arch.addBindValue(conversationId);
        } else {
            arch.prepare(QStringLiteral("UPDATE canvas_artifacts "
                                        "SET is_archived = 1, updated_at = ? "
                                        "WHERE conversation_id = ? "
                                        "  AND is_archived    = 0 "
                                        "  AND id            != ?"));
            arch.addBindValue(nowIso);
            arch.addBindValue(conversationId);
            arch.addBindValue(existingId);
        }
        if (!arch.exec()) {
            qCWarning(verzetaUi) << "CanvasService::openCanvas: archive prior active failed:"
                                 << arch.lastError().text();
            if (startedTxn)
                txnDb.rollback();
            return {};
        }
    }

    QString resultId;
    if (!existingId.isEmpty()) {
        // Upsert path — un-archive (if archived), bump revision, write
        // new content + updated_at.
        QSqlQuery upd(m_db.db());
        upd.prepare(QStringLiteral("UPDATE canvas_artifacts "
                                   "SET language    = ?, "
                                   "    content     = ?, "
                                   "    revision    = ?, "
                                   "    is_archived = 0, "
                                   "    updated_at  = ? "
                                   "WHERE id = ?"));
        upd.addBindValue(language);
        upd.addBindValue(content);
        upd.addBindValue(existingRevision + 1);
        upd.addBindValue(nowIso);
        upd.addBindValue(existingId);
        if (!upd.exec()) {
            qCWarning(verzetaUi) << "CanvasService::openCanvas: update existing row failed:"
                                 << upd.lastError().text();
            if (startedTxn)
                txnDb.rollback();
            return {};
        }
        resultId = existingId;
        qCInfo(verzetaUi) << "CanvasService::openCanvas: upserted existing canvas" << resultId
                          << "filename:" << filename << "rev:" << (existingRevision + 1)
                          << "(was archived:" << existingArchived << ")";
    } else {
        // Insert path — fresh row, revision 0.
        const QString id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        QSqlQuery ins(m_db.db());
        ins.prepare(QStringLiteral("INSERT INTO canvas_artifacts ("
                                   "  id, conversation_id, filename, language, content, "
                                   "  revision, is_archived, source_msg_id, created_at, updated_at"
                                   ") VALUES (?, ?, ?, ?, ?, 0, 0, ?, ?, ?)"));
        ins.addBindValue(id);
        ins.addBindValue(conversationId);
        ins.addBindValue(filename);
        ins.addBindValue(language);
        ins.addBindValue(content);
        ins.addBindValue(sourceMsgId);
        ins.addBindValue(nowIso);
        ins.addBindValue(nowIso);
        if (!ins.exec()) {
            qCWarning(verzetaUi) << "CanvasService::openCanvas: insert new row failed:"
                                 << ins.lastError().text();
            if (startedTxn)
                txnDb.rollback();
            return {};
        }
        resultId = id;
        qCInfo(verzetaUi) << "CanvasService::openCanvas: inserted canvas" << resultId
                          << "filename:" << filename << "language:" << language;
    }

    // Commit the atomic archive+write. On commit failure nothing lands
    // and the prior active canvas is preserved (no zero-canvas strand).
    if (startedTxn && !txnDb.commit()) {
        qCWarning(verzetaUi) << "CanvasService::openCanvas: commit failed, rolling back:"
                             << txnDb.lastError().text();
        txnDb.rollback();
        return {};
    }

    const QString writtenPath = writeDiskMirror(conversationId, filename, content);
    if (writtenPath.isEmpty()) {
        qCWarning(verzetaUi) << "CanvasService::openCanvas: disk mirror write failed —"
                             << "DB content is still safe; UI consumers should retry-save"
                             << "filename:" << filename;
        emit errorOccurred(
            QStringLiteral("The canvas was saved, but its copy on disk could not be written: %1")
                .arg(filename));
    }

    emit canvasOpened(conversationId, resultId);
    return resultId;
}

bool CanvasService::editCanvas(const QString& conversationId, const QString& newContent) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (conversationId.isEmpty())
        return false;

    // Find the active row.
    QString activeId;
    QString filename;
    int revision = 0;
    {
        QSqlQuery sel(m_db.db());
        sel.prepare(QStringLiteral("SELECT id, filename, revision FROM canvas_artifacts "
                                   "WHERE conversation_id = ? AND is_archived = 0 "
                                   "ORDER BY updated_at DESC LIMIT 1"));
        sel.addBindValue(conversationId);
        if (!sel.exec()) {
            qCWarning(verzetaUi) << "CanvasService::editCanvas: lookup failed:"
                                 << sel.lastError().text();
            return false;
        }
        if (!sel.next())
            return false;  // no active canvas
        activeId = sel.value(0).toString();
        filename = sel.value(1).toString();
        revision = sel.value(2).toInt();
    }

    const QString nowIso = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    QSqlQuery upd(m_db.db());
    upd.prepare(QStringLiteral("UPDATE canvas_artifacts "
                               "SET content = ?, revision = ?, updated_at = ? "
                               "WHERE id = ?"));
    upd.addBindValue(newContent);
    upd.addBindValue(revision + 1);
    upd.addBindValue(nowIso);
    upd.addBindValue(activeId);
    if (!upd.exec()) {
        qCWarning(verzetaUi) << "CanvasService::editCanvas: update failed:"
                             << upd.lastError().text();
        return false;
    }

    // Disk mirror — best-effort, same semantics as openCanvas.
    const QString writtenPath = writeDiskMirror(conversationId, filename, newContent);
    if (writtenPath.isEmpty()) {
        qCWarning(verzetaUi) << "CanvasService::editCanvas: disk mirror write failed —"
                             << "DB content is still safe; UI consumers should retry-save";
        emit errorOccurred(
            QStringLiteral("The canvas was saved, but its copy on disk could not be written: %1")
                .arg(filename));
    }

    qCInfo(verzetaUi) << "CanvasService::editCanvas:" << activeId << "rev:" << (revision + 1)
                      << "size:" << newContent.size();
    emit canvasUpdated(conversationId, activeId, revision + 1);
    return true;
}

QString
CanvasService::readCanvasSlice(const QString& conversationId, int startLine, int endLine) const {
    VERZETA_ASSERT_MAIN_THREAD();
    if (conversationId.isEmpty())
        return {};

    // Pull the active canvas content. We do NOT route through
    // activeCanvasFor's QVariantMap conversion to keep this hot path
    // cheap — a slice fetch shouldn't materialise the full row map.
    QString content;
    {
        QSqlQuery sel(m_db.db());
        sel.prepare(QStringLiteral("SELECT content FROM canvas_artifacts "
                                   "WHERE conversation_id = ? AND is_archived = 0 "
                                   "ORDER BY updated_at DESC LIMIT 1"));
        sel.addBindValue(conversationId);
        if (!sel.exec() || !sel.next())
            return {};
        content = sel.value(0).toString();
    }

    if (content.isEmpty())
        return {};

    // Split into lines. KeepEmptyParts because trailing blank lines
    // are real content the agent may legitimately want to read.
    const QStringList lines = content.split(QLatin1Char('\n'), Qt::KeepEmptyParts);
    const int total = lines.size();

    // 1-based start; clamp to range. 0 / negative → empty.
    if (startLine <= 0 || startLine > total)
        return {};

    // -1 means "to end of file"; otherwise must be >= startLine and
    // clamped to total.
    int effectiveEnd = (endLine == -1) ? total : endLine;
    if (effectiveEnd < startLine)
        return {};
    if (effectiveEnd > total)
        effectiveEnd = total;

    // mid(start, count) — note startLine is 1-based, mid is 0-based.
    return lines.mid(startLine - 1, effectiveEnd - startLine + 1).join(QLatin1Char('\n'));
}

bool CanvasService::closeCanvas(const QString& conversationId) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (conversationId.isEmpty())
        return false;

    // Find the active row (if any) so we can include its id in the
    // canvasClosed signal — UI subscribers need it to detect "is it
    // MY canvas that just closed".
    QString activeId;
    {
        QSqlQuery sel(m_db.db());
        sel.prepare(QStringLiteral("SELECT id FROM canvas_artifacts "
                                   "WHERE conversation_id = ? AND is_archived = 0 "
                                   "ORDER BY updated_at DESC LIMIT 1"));
        sel.addBindValue(conversationId);
        if (!sel.exec()) {
            qCWarning(verzetaUi) << "CanvasService::closeCanvas: lookup failed:"
                                 << sel.lastError().text();
            return false;
        }
        if (!sel.next()) {
            // No active canvas — caller (X close) has nothing to do.
            return false;
        }
        activeId = sel.value(0).toString();
    }

    const QString nowIso = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    QSqlQuery upd(m_db.db());
    upd.prepare(QStringLiteral("UPDATE canvas_artifacts "
                               "SET is_archived = 1, updated_at = ? "
                               "WHERE id = ?"));
    upd.addBindValue(nowIso);
    upd.addBindValue(activeId);
    if (!upd.exec()) {
        qCWarning(verzetaUi) << "CanvasService::closeCanvas: archive update failed:"
                             << upd.lastError().text();
        return false;
    }

    qCInfo(verzetaUi) << "CanvasService::closeCanvas: archived" << activeId;
    emit canvasClosed(conversationId, activeId);
    return true;
}

bool CanvasService::switchToCanvas(const QString& conversationId, const QString& canvasId) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (conversationId.isEmpty() || canvasId.isEmpty())
        return false;

    // Verify the target canvas exists in this conversation.
    QString targetFilename;
    bool targetIsArchived = false;
    {
        QSqlQuery sel(m_db.db());
        sel.prepare(QStringLiteral("SELECT filename, is_archived FROM canvas_artifacts "
                                   "WHERE id = ? AND conversation_id = ? LIMIT 1"));
        sel.addBindValue(canvasId);
        sel.addBindValue(conversationId);
        if (!sel.exec() || !sel.next()) {
            qCWarning(verzetaUi) << "CanvasService::switchToCanvas: target row not found —"
                                 << "convId:" << conversationId << "canvasId:" << canvasId;
            return false;
        }
        targetFilename = sel.value(0).toString();
        targetIsArchived = sel.value(1).toInt() != 0;
    }

    // Already the active canvas? No-op.
    if (!targetIsArchived) {
        // Verify it's the SOLE active row; otherwise something else is
        // wrong. Either way, no transition is needed.
        return true;
    }

    const QString nowIso = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);

    // Archive any current active row (different from target — target
    // is archived per the check above). Bump updated_at so the
    // activeCanvasFor lookup picks the right one.
    {
        QSqlQuery arch(m_db.db());
        arch.prepare(QStringLiteral("UPDATE canvas_artifacts "
                                    "SET is_archived = 1, updated_at = ? "
                                    "WHERE conversation_id = ? "
                                    "  AND is_archived    = 0"));
        arch.addBindValue(nowIso);
        arch.addBindValue(conversationId);
        if (!arch.exec()) {
            qCWarning(verzetaUi) << "CanvasService::switchToCanvas: archive prior failed:"
                                 << arch.lastError().text();
            return false;
        }
    }

    // Un-archive the target. Updated_at must be AFTER any archive
    // we just did so the active lookup picks the target.
    const QString nowIso2 = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    {
        QSqlQuery upd(m_db.db());
        upd.prepare(QStringLiteral("UPDATE canvas_artifacts "
                                   "SET is_archived = 0, updated_at = ? "
                                   "WHERE id = ?"));
        upd.addBindValue(nowIso2);
        upd.addBindValue(canvasId);
        if (!upd.exec()) {
            qCWarning(verzetaUi) << "CanvasService::switchToCanvas: un-archive failed:"
                                 << upd.lastError().text();
            return false;
        }
    }

    qCInfo(verzetaUi) << "CanvasService::switchToCanvas: switched to" << canvasId
                      << "filename:" << targetFilename;
    emit canvasOpened(conversationId, canvasId);
    return true;
}


namespace {

// Action ids — kept short + lowercase + underscored.
constexpr auto kActionValidate = "validate";
constexpr auto kActionFormat = "format";

bool isJsonLike(const QString& language) {
    const QString l = language.toLower();
    return l == QStringLiteral("json") || l == QStringLiteral("jsonc");
}

}  // namespace

QVariantList CanvasService::availableActionsForLanguage(const QString& language) const {
    VERZETA_ASSERT_MAIN_THREAD();

    QVariantList out;
    const bool jsonLike = isJsonLike(language);

    QVariantMap validate;
    validate.insert(QStringLiteral("id"), QString::fromLatin1(kActionValidate));
    validate.insert(QStringLiteral("label"), QStringLiteral("Validate"));
    validate.insert(QStringLiteral("description"),
                    jsonLike
                        ? QStringLiteral("Check the canvas for valid JSON syntax")
                        : QStringLiteral("No validator for this language, so it always passes."));
    validate.insert(QStringLiteral("enabled"), true);
    validate.insert(QStringLiteral("language"), language);
    out.append(validate);

    QVariantMap format;
    format.insert(QStringLiteral("id"), QString::fromLatin1(kActionFormat));
    format.insert(QStringLiteral("label"), QStringLiteral("Format"));
    format.insert(QStringLiteral("description"),
                  jsonLike ? QStringLiteral("Re-indent the canvas as pretty-printed JSON")
                           : QStringLiteral("No formatter is available for this language yet."));
    format.insert(QStringLiteral("enabled"), jsonLike);
    format.insert(QStringLiteral("language"), language);
    out.append(format);

    return out;
}


bool CanvasService::exportCanvasToFile(const QString& conversationId, const QString& absolutePath) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (conversationId.isEmpty() || absolutePath.isEmpty()) {
        emit errorOccurred(
            QStringLiteral("Export canvas: missing conversation or destination path"));
        return false;
    }

    const QVariantMap active = activeCanvasFor(conversationId);
    if (active.isEmpty()) {
        emit errorOccurred(QStringLiteral("Export canvas: no active canvas in this conversation"));
        return false;
    }

    const QString content = active.value(QStringLiteral("content")).toString();

    QSaveFile out(absolutePath);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit errorOccurred(QStringLiteral("Export canvas: cannot open %1 for writing (%2)")
                               .arg(absolutePath, out.errorString()));
        return false;
    }
    const QByteArray bytes = content.toUtf8();
    if (out.write(bytes) != bytes.size()) {
        emit errorOccurred(QStringLiteral("Export canvas: the file %1 was only partly written (%2)")
                               .arg(absolutePath, out.errorString()));
        out.cancelWriting();
        return false;
    }
    if (!out.commit()) {
        emit errorOccurred(QStringLiteral("Export canvas: could not save %1 (%2)")
                               .arg(absolutePath, out.errorString()));
        return false;
    }

    qCInfo(verzetaUi) << "CanvasService::exportCanvasToFile: wrote" << bytes.size() << "bytes to"
                      << absolutePath;
    return true;
}

QString CanvasService::suggestedExportName(const QString& conversationId) const {
    VERZETA_ASSERT_MAIN_THREAD();
    if (conversationId.isEmpty())
        return {};
    const QVariantMap active = activeCanvasFor(conversationId);
    if (active.isEmpty())
        return {};
    return active.value(QStringLiteral("filename")).toString();
}

QString CanvasService::diskMirrorPath(const QString& conversationId) const {
    VERZETA_ASSERT_MAIN_THREAD();
    if (conversationId.isEmpty() || !m_fileSvc)
        return {};

    const QVariantMap active = activeCanvasFor(conversationId);
    if (active.isEmpty())
        return {};

    const QString filename = active.value(QStringLiteral("filename")).toString();
    if (filename.isEmpty())
        return {};

    // Walk the folder chain to pick the same destination that
    // writeDiskMirror would have used. Project / org folder wins over
    // per-conversation when present.
    if (m_convSvc) {
        const QList<Folder> chain =
            const_cast<ConversationService*>(m_convSvc)->folderChainForConversation(conversationId);
        for (const Folder& f : chain) {
            if (f.isProject()) {
                m_fileSvc->setActiveProjectContext(f.id, f.name);
                const QString candidate =
                    QDir(m_fileSvc->activeProjectDir()).absoluteFilePath(filename);
                if (QFileInfo::exists(candidate))
                    return candidate;
                break;
            }
        }
    }

    // Fallback / standalone conversation candidate.
    m_fileSvc->setActiveConversation(conversationId);
    const QString candidate = QDir(m_fileSvc->activeProjectDir()).absoluteFilePath(filename);
    return QFileInfo::exists(candidate) ? candidate : QString();
}

// ---------------------------------------------------------------------------

QVariantMap CanvasService::performAction(const QString& conversationId, const QString& actionId) {
    VERZETA_ASSERT_MAIN_THREAD();

    QVariantMap result;
    result.insert(QStringLiteral("ok"), false);
    result.insert(QStringLiteral("mutated"), false);
    result.insert(QStringLiteral("message"), QString());

    if (conversationId.isEmpty()) {
        result.insert(QStringLiteral("message"), QStringLiteral("No active conversation"));
        return result;
    }

    const QVariantMap active = activeCanvasFor(conversationId);
    if (active.isEmpty()) {
        result.insert(QStringLiteral("message"), QStringLiteral("No active canvas"));
        return result;
    }

    const QString language = active.value(QStringLiteral("language")).toString();
    const QString content = active.value(QStringLiteral("content")).toString();

    if (actionId == QString::fromLatin1(kActionValidate)) {
        if (isJsonLike(language)) {
            QJsonParseError err;
            QJsonDocument::fromJson(content.toUtf8(), &err);
            if (err.error == QJsonParseError::NoError) {
                result.insert(QStringLiteral("ok"), true);
                result.insert(QStringLiteral("message"), QStringLiteral("Valid JSON."));
            } else {
                // Convert byte offset to a 1-based line number for
                // user-friendly diagnostics.
                int line = 1;
                for (int i = 0; i < err.offset && i < content.size(); ++i) {
                    if (content.at(i) == QLatin1Char('\n'))
                        ++line;
                }
                result.insert(QStringLiteral("ok"), false);
                result.insert(QStringLiteral("message"),
                              QStringLiteral("JSON parse error at line %1: %2")
                                  .arg(line)
                                  .arg(err.errorString()));
                result.insert(QStringLiteral("errorLine"), line);
                result.insert(QStringLiteral("errorOffset"), err.offset);
            }
        } else {
            // No validator implemented for this language in v1; treat
            // it as "always passes" so the menu entry remains useful
            // (the user gets a positive signal that nothing's broken
            // structurally that we can detect).
            result.insert(QStringLiteral("ok"), true);
            result.insert(QStringLiteral("message"),
                          QStringLiteral("No validator is available for "
                                         "%1, so it was treated as valid.")
                              .arg(language.isEmpty() ? QStringLiteral("plaintext") : language));
        }
        return result;
    }

    if (actionId == QString::fromLatin1(kActionFormat)) {
        if (!isJsonLike(language)) {
            result.insert(QStringLiteral("message"),
                          QStringLiteral("Format not available for %1 yet.").arg(language));
            return result;
        }

        QJsonParseError err;
        const QJsonDocument doc = QJsonDocument::fromJson(content.toUtf8(), &err);
        if (err.error != QJsonParseError::NoError) {
            int line = 1;
            for (int i = 0; i < err.offset && i < content.size(); ++i) {
                if (content.at(i) == QLatin1Char('\n'))
                    ++line;
            }
            result.insert(QStringLiteral("message"),
                          QStringLiteral("Cannot format invalid JSON: "
                                         "parse error at line %1 (%2)")
                              .arg(line)
                              .arg(err.errorString()));
            result.insert(QStringLiteral("errorLine"), line);
            return result;
        }

        const QString formatted = QString::fromUtf8(doc.toJson(QJsonDocument::Indented));
        // Strip the trailing newline QJsonDocument always appends —
        // the canvas-author may want their own newline policy and we
        // should not silently change it relative to the input shape.
        QString clean = formatted;
        while (clean.endsWith(QLatin1Char('\n'))) {
            clean.chop(1);
        }
        if (content.endsWith(QLatin1Char('\n'))) {
            clean.append(QLatin1Char('\n'));
        }

        if (clean == content) {
            result.insert(QStringLiteral("ok"), true);
            result.insert(QStringLiteral("message"), QStringLiteral("JSON is already formatted."));
            return result;
        }

        if (!editCanvas(conversationId, clean)) {
            result.insert(QStringLiteral("message"),
                          QStringLiteral("The canvas was formatted, but the "
                                         "result could not be saved."));
            return result;
        }
        result.insert(QStringLiteral("ok"), true);
        result.insert(QStringLiteral("mutated"), true);
        result.insert(QStringLiteral("message"),
                      QStringLiteral("Formatted as pretty-printed JSON."));
        return result;
    }

    result.insert(QStringLiteral("message"), QStringLiteral("Unknown action: %1").arg(actionId));
    return result;
}

namespace {

constexpr int kAutoPromoteMinChars = 500;

QString languageForFilename(const QString& filename) {
    const QString lower = filename.toLower();
    // Extensionless / by-name build + config files a project commonly ships.
    if (lower == QStringLiteral("cmakelists.txt"))
        return QStringLiteral("cmake");
    if (lower == QStringLiteral("dockerfile"))
        return QStringLiteral("dockerfile");
    if (lower == QStringLiteral("makefile"))
        return QStringLiteral("makefile");
    if (lower == QStringLiteral("gnumakefile"))
        return QStringLiteral("makefile");

    const int dot = lower.lastIndexOf(QLatin1Char('.'));
    if (dot < 0 || dot == lower.size() - 1)
        return {};
    const QString ext = lower.mid(dot + 1);

    // Extension -> syntax-highlight language. Presence in this map is also
    // the auto-promote allowlist gate (a recognised text/code/markup file is
    // promoted to a canvas). The language string only drives highlighting —
    // an unknown definition degrades to no highlighting, never an error — so
    // erring toward MORE extensions is safe and matches user expectation that
    // any source/markup/config file they generate is openable in the canvas.
    static const QHash<QString, QString> map = {
        // Docs / markup
        {QStringLiteral("md"), QStringLiteral("markdown")},
        {QStringLiteral("markdown"), QStringLiteral("markdown")},
        {QStringLiteral("rst"), QStringLiteral("plaintext")},
        {QStringLiteral("tex"), QStringLiteral("latex")},
        {QStringLiteral("txt"), QStringLiteral("plaintext")},
        // Web
        {QStringLiteral("html"), QStringLiteral("html")},
        {QStringLiteral("htm"), QStringLiteral("html")},
        {QStringLiteral("xhtml"), QStringLiteral("html")},
        {QStringLiteral("css"), QStringLiteral("css")},
        {QStringLiteral("scss"), QStringLiteral("scss")},
        {QStringLiteral("less"), QStringLiteral("less")},
        {QStringLiteral("vue"), QStringLiteral("html")},
        {QStringLiteral("svelte"), QStringLiteral("html")},
        // Data / config / query
        {QStringLiteral("json"), QStringLiteral("json")},
        {QStringLiteral("jsonc"), QStringLiteral("json")},
        {QStringLiteral("yaml"), QStringLiteral("yaml")},
        {QStringLiteral("yml"), QStringLiteral("yaml")},
        {QStringLiteral("toml"), QStringLiteral("toml")},
        {QStringLiteral("ini"), QStringLiteral("ini")},
        {QStringLiteral("conf"), QStringLiteral("ini")},
        {QStringLiteral("cfg"), QStringLiteral("ini")},
        {QStringLiteral("properties"), QStringLiteral("ini")},
        {QStringLiteral("env"), QStringLiteral("bash")},
        {QStringLiteral("xml"), QStringLiteral("xml")},
        {QStringLiteral("svg"), QStringLiteral("xml")},
        {QStringLiteral("xsd"), QStringLiteral("xml")},
        {QStringLiteral("xsl"), QStringLiteral("xml")},
        {QStringLiteral("csv"), QStringLiteral("plaintext")},
        {QStringLiteral("tsv"), QStringLiteral("plaintext")},
        {QStringLiteral("sql"), QStringLiteral("sql")},
        {QStringLiteral("graphql"), QStringLiteral("graphql")},
        {QStringLiteral("gql"), QStringLiteral("graphql")},
        {QStringLiteral("proto"), QStringLiteral("plaintext")},
        // Python
        {QStringLiteral("py"), QStringLiteral("python")},
        {QStringLiteral("pyw"), QStringLiteral("python")},
        {QStringLiteral("pyi"), QStringLiteral("python")},
        // JS / TS
        {QStringLiteral("ts"), QStringLiteral("typescript")},
        {QStringLiteral("tsx"), QStringLiteral("typescript")},
        {QStringLiteral("js"), QStringLiteral("javascript")},
        {QStringLiteral("jsx"), QStringLiteral("javascript")},
        {QStringLiteral("mjs"), QStringLiteral("javascript")},
        {QStringLiteral("cjs"), QStringLiteral("javascript")},
        // C / C++ — sources, headers, modules, inline
        {QStringLiteral("c"), QStringLiteral("c")},
        {QStringLiteral("cpp"), QStringLiteral("cpp")},
        {QStringLiteral("cc"), QStringLiteral("cpp")},
        {QStringLiteral("cxx"), QStringLiteral("cpp")},
        {QStringLiteral("c++"), QStringLiteral("cpp")},
        {QStringLiteral("h"), QStringLiteral("cpp")},
        {QStringLiteral("hpp"), QStringLiteral("cpp")},
        {QStringLiteral("hh"), QStringLiteral("cpp")},
        {QStringLiteral("hxx"), QStringLiteral("cpp")},
        {QStringLiteral("h++"), QStringLiteral("cpp")},
        {QStringLiteral("ipp"), QStringLiteral("cpp")},
        {QStringLiteral("inl"), QStringLiteral("cpp")},
        {QStringLiteral("tpp"), QStringLiteral("cpp")},
        {QStringLiteral("cppm"), QStringLiteral("cpp")},
        {QStringLiteral("ixx"), QStringLiteral("cpp")},
        // Rust / Go
        {QStringLiteral("rs"), QStringLiteral("rust")},
        {QStringLiteral("go"), QStringLiteral("go")},
        // JVM
        {QStringLiteral("java"), QStringLiteral("java")},
        {QStringLiteral("kt"), QStringLiteral("kotlin")},
        {QStringLiteral("kts"), QStringLiteral("kotlin")},
        {QStringLiteral("scala"), QStringLiteral("scala")},
        {QStringLiteral("groovy"), QStringLiteral("groovy")},
        {QStringLiteral("gradle"), QStringLiteral("groovy")},
        // .NET
        {QStringLiteral("cs"), QStringLiteral("csharp")},
        {QStringLiteral("fs"), QStringLiteral("fsharp")},
        {QStringLiteral("fsx"), QStringLiteral("fsharp")},
        // Apple
        {QStringLiteral("swift"), QStringLiteral("swift")},
        {QStringLiteral("m"), QStringLiteral("objective-c")},
        {QStringLiteral("mm"), QStringLiteral("objective-c")},
        // Scripting
        {QStringLiteral("rb"), QStringLiteral("ruby")},
        {QStringLiteral("php"), QStringLiteral("php")},
        {QStringLiteral("pl"), QStringLiteral("perl")},
        {QStringLiteral("pm"), QStringLiteral("perl")},
        {QStringLiteral("lua"), QStringLiteral("lua")},
        {QStringLiteral("r"), QStringLiteral("r")},
        {QStringLiteral("dart"), QStringLiteral("dart")},
        {QStringLiteral("jl"), QStringLiteral("julia")},
        // Functional / systems / other
        {QStringLiteral("hs"), QStringLiteral("haskell")},
        {QStringLiteral("ex"), QStringLiteral("elixir")},
        {QStringLiteral("exs"), QStringLiteral("elixir")},
        {QStringLiteral("erl"), QStringLiteral("erlang")},
        {QStringLiteral("clj"), QStringLiteral("clojure")},
        {QStringLiteral("cljs"), QStringLiteral("clojure")},
        {QStringLiteral("zig"), QStringLiteral("zig")},
        {QStringLiteral("nim"), QStringLiteral("nim")},
        {QStringLiteral("vala"), QStringLiteral("vala")},
        // Shell
        {QStringLiteral("sh"), QStringLiteral("shell")},
        {QStringLiteral("bash"), QStringLiteral("shell")},
        {QStringLiteral("zsh"), QStringLiteral("shell")},
        {QStringLiteral("fish"), QStringLiteral("shell")},
        {QStringLiteral("ps1"), QStringLiteral("powershell")},
        // Qt / build
        {QStringLiteral("qml"), QStringLiteral("qml")},
        {QStringLiteral("cmake"), QStringLiteral("cmake")},
    };
    return map.value(ext);
}

}  // namespace

QString CanvasService::tryAutoPromote(const QString& conversationId,
                                      const QString& filename,
                                      const QString& content) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (conversationId.isEmpty() || filename.isEmpty())
        return {};
    if (content.size() < kAutoPromoteMinChars) {
        qCDebug(verzetaUi) << "CanvasService::tryAutoPromote: skip (size <" << kAutoPromoteMinChars
                           << ") —" << filename;
        return {};
    }

    const QString language = languageForFilename(filename);
    if (language.isEmpty()) {
        qCDebug(verzetaUi) << "CanvasService::tryAutoPromote: skip (extension not in"
                              " allowlist) —"
                           << filename;
        return {};
    }

    qCInfo(verzetaUi) << "CanvasService::tryAutoPromote: promoting" << filename << "("
                      << content.size() << "chars," << language << ") —"
                      << "convId:" << conversationId;
    return openCanvas(conversationId, filename, language, content);
}

QString CanvasService::openCanvasFromFile(const QString& conversationId,
                                          const QString& absolutePath) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (conversationId.isEmpty() || absolutePath.isEmpty()) {
        qCWarning(verzetaUi) << "CanvasService::openCanvasFromFile: required arg missing —"
                             << "convId:" << conversationId << "path:" << absolutePath;
        return {};
    }

    const QFileInfo info(absolutePath);
    if (!info.exists() || !info.isFile()) {
        qCWarning(verzetaUi) << "CanvasService::openCanvasFromFile: not a regular file:"
                             << absolutePath;
        return {};
    }
    // Same order-of-magnitude bound as FileService::saveGeneratedFile —
    // the canvas is a text-editing surface, not a binary viewer.
    constexpr qint64 kMaxOpenBytes = 10 * 1024 * 1024;
    if (info.size() > kMaxOpenBytes) {
        qCWarning(verzetaUi) << "CanvasService::openCanvasFromFile: file exceeds 10 MB —"
                             << absolutePath << "(" << info.size() << "bytes )";
        return {};
    }

    QFile f(absolutePath);
    if (!f.open(QIODevice::ReadOnly)) {
        qCWarning(verzetaUi) << "CanvasService::openCanvasFromFile: cannot read" << absolutePath
                             << "—" << f.errorString();
        return {};
    }
    const QByteArray bytes = f.readAll();
    f.close();
    if (bytes.contains('\0')) {
        qCWarning(verzetaUi) << "CanvasService::openCanvasFromFile: refusing binary file"
                             << absolutePath;
        return {};
    }

    const QString content = QString::fromUtf8(bytes);
    const QString filename = info.fileName();
    // User-initiated open: an unmapped extension falls back to
    // plaintext instead of being refused (unlike the auto-promote
    // allowlist, which gates an implicit promotion).
    QString language = languageForFilename(filename);
    if (language.isEmpty())
        language = QStringLiteral("plaintext");

    qCInfo(verzetaUi) << "CanvasService::openCanvasFromFile: opening" << filename << "("
                      << content.size() << "chars," << language << ") —"
                      << "convId:" << conversationId;
    return openCanvas(conversationId, filename, language, content);
}

QString CanvasService::openCanvasFromWorkspace(const QString& conversationId,
                                               const QString& filename) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (conversationId.isEmpty() || filename.isEmpty() || !m_fileSvc) {
        return {};
    }
    const QString dir = workspaceDirForConversation(conversationId);
    if (dir.isEmpty())
        return {};

    // Resolve the name safely within the workspace (subdirectories kept,
    // traversal/absolute rejected). A mount-hosted file has no local copy
    // here, so isFile() is false and we return empty — the open_canvas
    // tool then asks the agent to read_file (mount-aware, off the main
    // thread) and re-open WITH content, rather than blocking on a wire
    // read from this main-thread call.
    const QString rel = FileService::sanitiseRelativePath(filename);
    if (rel.isEmpty())
        return {};
    const QString abs = QDir(dir).absoluteFilePath(rel);
    if (!QFileInfo(abs).isFile())
        return {};
    return openCanvasFromFile(conversationId, abs);
}

QString CanvasService::retargetAndEdit(const QString& conversationId,
                                       const QString& filename,
                                       const QString& newContent) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (conversationId.isEmpty() || filename.isEmpty())
        return {};

    // Prefer an existing canvas row's language (re-activate it); otherwise
    // adopt a workspace file of that name with a language inferred from its
    // extension. A name that matches neither is not created here.
    QString language;
    {
        QSqlQuery sel(m_db.db());
        sel.prepare(QStringLiteral("SELECT language FROM canvas_artifacts "
                                   "WHERE conversation_id = ? AND filename = ? "
                                   "ORDER BY updated_at DESC LIMIT 1"));
        sel.addBindValue(conversationId);
        sel.addBindValue(filename);
        if (sel.exec() && sel.next()) {
            language = sel.value(0).toString();
        }
    }
    if (language.isEmpty()) {
        const QString dir = workspaceDirForConversation(conversationId);
        const QString rel = FileService::sanitiseRelativePath(filename);
        if (dir.isEmpty() || rel.isEmpty())
            return {};
        if (!QFileInfo(QDir(dir).absoluteFilePath(rel)).isFile())
            return {};
        language = languageForFilename(filename);
        if (language.isEmpty())
            language = QStringLiteral("plaintext");
    }
    // openCanvas upserts by filename (re-activate + bump revision + set
    // content) or inserts a fresh row, archiving other actives — atomic.
    return openCanvas(conversationId, filename, language, newContent);
}

QString CanvasService::workspaceDirForConversation(const QString& conversationId) const {
    if (conversationId.isEmpty() || !m_fileSvc)
        return {};
    if (m_convSvc) {
        const QList<Folder> chain =
            const_cast<ConversationService*>(m_convSvc)->folderChainForConversation(conversationId);
        for (const Folder& f : chain) {
            if (f.isProject()) {
                m_fileSvc->setActiveProjectContext(f.id, f.name);
                return m_fileSvc->activeProjectDir();
            }
        }
    }
    m_fileSvc->setActiveConversation(conversationId);
    return m_fileSvc->activeProjectDir();
}

QString CanvasService::writeDiskMirror(const QString& conversationId,
                                       const QString& filename,
                                       const QString& content) const {
    if (!m_fileSvc) {
        qCWarning(verzetaUi) << "CanvasService::writeDiskMirror: FileService null —"
                             << "skipping disk mirror";
        return {};
    }

    // Resolve the destination directory the same way
    // `ChatController::activeArtifactsPath` does — project-scoped if the
    // conversation lives inside a project/org folder, per-conversation
    // otherwise (shared with openCanvasFromWorkspace).
    const QString dest = workspaceDirForConversation(conversationId);
    // Re-entry guard — see m_inWriteMirror comment in the header.
    const bool savedGuard = m_inWriteMirror;
    const_cast<CanvasService*>(this)->m_inWriteMirror = true;
    const QString result = m_fileSvc->saveGeneratedFile(filename, content, dest);
    const_cast<CanvasService*>(this)->m_inWriteMirror = savedGuard;
    return result;
}
