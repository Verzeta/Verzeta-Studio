// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file rag-service.cpp
 * @brief Implementation of the RAG pipeline: chunking, embedding storage,
 *        cosine similarity retrieval, and prompt augmentation.
 * @layer Service (Search Index)
 * @dependencies DbManager, EmbeddingWorker, Qt6::Sql
 */


#include "rag-service.h"

#include "utils/logger.h"

#include <QThread>

#include <algorithm>
#include <cmath>
#include <QDateTime>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QRegularExpression>
#include <QSqlError>
#include <QSqlQuery>
#include <QUrl>
#include <QUuid>

// Chunk parameters
static constexpr int kDefaultChunkSize = 512;  ///< Target chunk size in characters.
static constexpr int kDefaultOverlap = 64;     ///< Overlap between chunks in characters.

// ---------------------------------------------------------------------------
// Constructor / Destructor
// ---------------------------------------------------------------------------

/*
 * @brief Constructs the RagService and wires up embedding signals.
 * @param db      Open database.
 * @param embedder EmbeddingWorker instance.
 * @param parent  Optional Qt parent.
 */
RagService::RagService(DbManager& db, EmbeddingWorker& embedder, QObject* parent)
    : QObject(parent)
    , m_db(db)
    , m_bulkEmbedder(embedder)
    , m_queryEmbedder(embedder)
    , m_vectors(db, QStringLiteral("embeddings"), QStringLiteral("vec_embeddings")) {
    wireEmbedderSignals();
}

RagService::RagService(DbManager& db,
                       EmbeddingWorker& bulkEmbedder,
                       EmbeddingWorker& queryEmbedder,
                       QObject* parent)
    : QObject(parent)
    , m_db(db)
    , m_bulkEmbedder(bulkEmbedder)
    , m_queryEmbedder(queryEmbedder)
    , m_vectors(db, QStringLiteral("embeddings"), QStringLiteral("vec_embeddings")) {
    wireEmbedderSignals();
}

void RagService::wireEmbedderSignals() {
    // Bulk worker → storage slots; query worker → query slots. The worker's
    // completion signal IS the continuation — queued (worker → main), no
    // blocking, no timer. With one shared worker (single-worker ctor) both slot
    // sets bind to it and the id-prefix guards route ("ragq:" = query). With
    // separate workers (G4) routing is by which worker emits.
    connect(&m_bulkEmbedder, &EmbeddingWorker::embeddingReady, this, &RagService::onEmbeddingReady);
    connect(&m_bulkEmbedder, &EmbeddingWorker::errorOccurred, this, &RagService::onEmbeddingError);
    connect(&m_queryEmbedder,
            &EmbeddingWorker::embeddingReady,
            this,
            &RagService::onQueryEmbeddingReady);
    connect(&m_queryEmbedder,
            &EmbeddingWorker::errorOccurred,
            this,
            &RagService::onQueryEmbeddingError);
}

RagService::~RagService() = default;

// ---------------------------------------------------------------------------
// Indexing
// ---------------------------------------------------------------------------
//
// RagService is a stateless engine: it has NO global enable flag. Whether RAG
// runs for a given turn/message is decided PER CONVERSATION by the callers
// (AppController::messageAdded and ConversationRun::maybeBeginRagPrefetch read
// llm_config.rag_enabled). Global knowledge-base documents are always indexable.
// If no embedding provider responds, retrieval simply yields nothing (N4
// graceful degradation), so no service-level "enabled" gate is needed.

/*
 * @brief Indexes a document by chunking it and scheduling embedding computation.
 * @param docPath Absolute path to the document (used as source ID).
 * @param content Full text content.
 */
void RagService::indexDocument(const QString& docPath,
                               const QString& content,
                               const QString& ownerScope) {
    if (docPath.isEmpty() || content.isEmpty()) {
        emit indexingError(docPath, QStringLiteral("Empty path or content"));
        return;
    }

    // Insert/replace document record
    const QString docId = QUuid::createUuid().toString(QUuid::WithoutBraces);
    {
        QSqlQuery q(m_db.db());
        q.prepare(
            QStringLiteral("INSERT OR REPLACE INTO documents(id, name, path, content, mime_type, "
                           "created_at, indexed_at) VALUES(?, ?, ?, ?, ?, ?, ?)"));
        q.addBindValue(docId);
        q.addBindValue(QFileInfo(docPath).fileName());
        q.addBindValue(docPath);
        q.addBindValue(content);
        q.addBindValue(QStringLiteral("text/plain"));
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        q.addBindValue(now);
        q.addBindValue(now);
        if (!q.exec()) {
            qCWarning(verzetaRag) << "RagService: failed to insert document"
                                  << q.lastError().text();
            emit indexingError(docPath, q.lastError().text());
            return;
        }
    }

    // Chunk the text and schedule embeddings
    QStringList chunks;
    chunkText(content, chunks, kDefaultChunkSize, kDefaultOverlap);

    m_pendingChunks[docId] = chunks.size();

    for (int i = 0; i < chunks.size(); ++i) {
        const QString chunkId = QStringLiteral("%1::%2::document").arg(docId).arg(i);
        // Remember the text + scope so onEmbeddingReady can persist
        // chunk_text and owner_scope.
        m_pendingText.insert(chunkId, chunks[i]);
        m_pendingScope.insert(chunkId, ownerScope);
        QMetaObject::invokeMethod(&m_bulkEmbedder,
                                  "computeEmbedding",
                                  Qt::QueuedConnection,
                                  Q_ARG(QString, chunkId),
                                  Q_ARG(QString, chunks[i]),
                                  Q_ARG(QString, QStringLiteral("document")));
    }

    qCInfo(verzetaRag) << "RagService: indexing document" << docPath << "as" << chunks.size()
                       << "chunks";
}

bool RagService::indexDocumentFile(const QString& filePath) {
    // Accept either a plain path or a file:// URL (QML FileDialog yields URLs).
    QString localPath = filePath;
    if (localPath.startsWith(QStringLiteral("file:"))) {
        localPath = QUrl(localPath).toLocalFile();
    }
    QFile f(localPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qCWarning(verzetaRag) << "RagService::indexDocumentFile: cannot open" << localPath << ":"
                              << f.errorString();
        emit indexingError(localPath, QStringLiteral("Cannot open file: %1").arg(f.errorString()));
        return false;
    }
    const QString content = QString::fromUtf8(f.readAll());
    f.close();
    if (content.trimmed().isEmpty()) {
        emit indexingError(localPath, QStringLiteral("File is empty"));
        return false;
    }
    indexDocument(localPath, content);
    return true;
}

/*
 * @brief Indexes a conversation message by scheduling its embedding.
 * @param messageId UUID of the message.
 * @param text      Message text.
 */
void RagService::indexMessage(const QString& messageId,
                              const QString& text,
                              const QString& ownerScope) {
    if (messageId.isEmpty() || text.isEmpty()) {
        return;
    }
    // Strip @-mention sigils before embedding/storing. Group-chat messages carry
    // routing tags (e.g. "@Bob, draft Step 1"); if those are retrieved and
    // injected back as context, the model parrots them — including tagging
    // itself — instead of acting. We keep the name, drop the "@" so meaning is
    // preserved but the actionable routing token is gone. (Messages only;
    // documents may legitimately contain "@".)
    const QString clean = stripMentions(text).trimmed();
    if (clean.isEmpty()) {
        return;  // nothing left to index after stripping
    }
    const QString chunkId = QStringLiteral("%1::0::message").arg(messageId);
    // Remember the text + scope so onEmbeddingReady can persist chunk_text
    // and owner_scope.
    m_pendingText.insert(chunkId, clean);
    m_pendingScope.insert(chunkId, ownerScope);
    QMetaObject::invokeMethod(&m_bulkEmbedder,
                              "computeEmbedding",
                              Qt::QueuedConnection,
                              Q_ARG(QString, chunkId),
                              Q_ARG(QString, clean),
                              Q_ARG(QString, QStringLiteral("message")));
}

QString RagService::stripMentions(const QString& text) {
    // Reuse the cascade/routing @-mention shape (@ + identifier). Replace
    // "@Name" with "Name" so the content reads naturally without the literal
    // routing sigil the model would otherwise echo.
    static const QRegularExpression kMention(QStringLiteral("@([A-Za-z0-9_]+)"));
    QString out = text;
    out.replace(kMention, QStringLiteral("\\1"));
    return out;
}

// ---------------------------------------------------------------------------
// Retrieval
// ---------------------------------------------------------------------------

/*
 * @brief Retrieves the top-K chunks most relevant to a query.
 * @param query Search query text.
 * @param topK  Maximum number of results.
 * @return Sorted list of RagChunk (highest similarity first).
 */
QList<RagChunk> RagService::retrieve(const QString& query, int topK) {
    if (query.isEmpty()) {
        return {};
    }

    QVector<float> queryEmbedding;
    QString embedError;
    bool done = false;

    const auto connOk = connect(
        &m_queryEmbedder,
        &EmbeddingWorker::embeddingReady,
        this,
        [&](const QString& id, const QVector<float>& emb, const QString& /*modelId*/) {
            if (id != QStringLiteral("__query__"))
                return;
            queryEmbedding = emb;
            done = true;
        },
        Qt::DirectConnection);
    const auto connErr = connect(
        &m_queryEmbedder,
        &EmbeddingWorker::errorOccurred,
        this,
        [&](const QString& id, const QString& err) {
            if (id != QStringLiteral("__query__"))
                return;
            embedError = err;
            done = true;
        },
        Qt::DirectConnection);

    QMetaObject::invokeMethod(&m_queryEmbedder,
                              "computeEmbedding",
                              m_queryEmbedder.thread() == QThread::currentThread()
                                  ? Qt::DirectConnection
                                  : Qt::BlockingQueuedConnection,
                              Q_ARG(QString, QStringLiteral("__query__")),
                              Q_ARG(QString, query),
                              Q_ARG(QString, QStringLiteral("query")));

    disconnect(connOk);
    disconnect(connErr);

    if (queryEmbedding.isEmpty()) {
        qCWarning(verzetaRag) << "RagService: query embedding failed:"
                              << (embedError.isEmpty() ? QStringLiteral("(no result delivered)")
                                                       : embedError);
        return {};
    }

    // Load all stored embeddings
    const auto stored = loadEmbeddings();
    if (stored.isEmpty()) {
        return {};
    }

    // Compute cosine similarities and rank
    QList<QPair<float, QString>> scored;  // (score, embeddingId)
    scored.reserve(stored.size());
    for (const auto& [embId, embVec] : stored) {
        const float sim = VectorStore::cosineSimilarity(queryEmbedding, embVec);
        scored.append({sim, embId});
    }

    std::sort(scored.begin(), scored.end(), [](const auto& a, const auto& b) {
        return a.first > b.first;
    });

    // Load text content for top-K chunks
    QList<RagChunk> results;
    for (int i = 0; i < qMin(topK, static_cast<int>(scored.size())); ++i) {
        const QString& embId = scored[i].second;
        // embId format: "sourceId::chunkIndex::sourceType"
        const QStringList parts = embId.split(QStringLiteral("::"));
        if (parts.size() < 3) {
            continue;
        }
        const QString sourceId = parts[0];
        const QString sourceType = parts[2];

        // Fetch chunk text from DB
        QSqlQuery q(m_db.db());
        q.prepare(QStringLiteral("SELECT chunk_text FROM embeddings WHERE id = ?"));
        q.addBindValue(embId);
        if (!q.exec() || !q.next()) {
            continue;
        }

        RagChunk chunk;
        chunk.sourceId = sourceId;
        chunk.sourceType = sourceType;
        chunk.text = q.value(0).toString();
        chunk.score = scored[i].first;
        results.append(chunk);
    }

    return results;
}

/*
 * @brief Augments a system prompt with retrieved context chunks.
 * @param userQuery  User query text.
 * @param basePrompt Base system prompt.
 * @param topK       Number of context chunks.
 * @return Augmented prompt with "## Relevant Context" section appended.
 */
QString RagService::augmentPrompt(const QString& userQuery, const QString& basePrompt, int topK) {
    const QList<RagChunk> chunks = retrieve(userQuery, topK);
    return basePrompt + formatChunksAsContext(chunks);
}

QString RagService::formatChunksAsContext(const QList<RagChunk>& chunks) {
    if (chunks.isEmpty()) {
        return {};
    }
    QString context = QStringLiteral("\n\n## Relevant Context\n");
    for (const RagChunk& chunk : chunks) {
        // Strip @-mention sigils from retrieved context so an agent can't
        // parrot stale routing or self-tag from older (pre-strip) corpus
        // rows. Indexing also strips on write; this is the read-side guard
        // so even legacy rows are safe.
        context += stripMentions(chunk.text);
        context += QStringLiteral("\n---\n");
    }
    // Remove trailing separator.
    if (context.endsWith(QStringLiteral("\n---\n"))) {
        context.chop(5);
    }
    return context;
}

// ---------------------------------------------------------------------------
// Async retrieval (signal-driven; the turn-path entry point)
// ---------------------------------------------------------------------------

quint64 RagService::retrieveAsync(const QString& query, const QString& conversationId, int topK) {
    if (query.isEmpty()) {
        return 0;
    }
    const quint64 queryId = ++m_nextQueryId;
    PendingQuery pq;
    pq.scopes = computeVisibility(conversationId);
    pq.topK = topK;
    m_pendingQueries.insert(queryId, pq);

    // Kick the query embed on the worker thread (queued — never blocks the
    // caller). The "ragq:" id has no "::" so the storage slot ignores it; the
    // worker's embeddingReady/errorOccurred IS the continuation (no timer).
    QMetaObject::invokeMethod(&m_queryEmbedder,
                              "computeEmbedding",
                              Qt::QueuedConnection,
                              Q_ARG(QString, QStringLiteral("ragq:%1").arg(queryId)),
                              Q_ARG(QString, query),
                              Q_ARG(QString, QStringLiteral("query")));
    return queryId;
}

quint64 RagService::retrieveAllAsync(const QString& query, int topK) {
    if (query.isEmpty()) {
        return 0;
    }
    const quint64 queryId = ++m_nextQueryId;
    PendingQuery pq;
    pq.scopes = {};  // empty = no scope filter → search the whole corpus
    pq.topK = topK;
    m_pendingQueries.insert(queryId, pq);
    QMetaObject::invokeMethod(&m_queryEmbedder,
                              "computeEmbedding",
                              Qt::QueuedConnection,
                              Q_ARG(QString, QStringLiteral("ragq:%1").arg(queryId)),
                              Q_ARG(QString, query),
                              Q_ARG(QString, QStringLiteral("query")));
    return queryId;
}

void RagService::onQueryEmbeddingReady(const QString& id,
                                       const QVector<float>& embedding,
                                       const QString& modelId) {
    if (!id.startsWith(QStringLiteral("ragq:"))) {
        return;
    }
    bool ok = false;
    const quint64 queryId = id.mid(5).toULongLong(&ok);
    if (!ok) {
        return;
    }
    const auto it = m_pendingQueries.find(queryId);
    if (it == m_pendingQueries.end()) {
        return;  // already delivered / cancelled
    }
    const PendingQuery pq = it.value();
    m_pendingQueries.erase(it);

    QList<RagChunk> chunks;
    if (!embedding.isEmpty()) {
        chunks = scoreScoped(embedding, modelId, pq.scopes, pq.topK);
    }
    // Observability: one line per retrieval so RAG augmentation is visible
    // (it is otherwise injected silently into the prompt). Covers every
    // consumer — turn augmentation, search, and agent memory.
    qCDebug(verzetaRag).nospace()
        << "RAG: retrieved " << chunks.size() << " chunk(s) [queryId " << queryId << ", model "
        << (modelId.isEmpty() ? QStringLiteral("?") : modelId) << ", scopes "
        << (pq.scopes.isEmpty() ? QStringLiteral("(all)") : pq.scopes.join(QLatin1Char(',')))
        << ", topScore "
        << (chunks.isEmpty() ? QStringLiteral("-")
                             : QString::number(static_cast<double>(chunks.first().score), 'f', 3))
        << ", backend "
        << (m_db.isVectorSearchAvailable() ? QStringLiteral("vec0") : QStringLiteral("brute-force"))
        << "]";
    emit retrievalReady(queryId, chunks);
}

void RagService::onQueryEmbeddingError(const QString& id, const QString& error) {
    if (!id.startsWith(QStringLiteral("ragq:"))) {
        return;
    }
    bool ok = false;
    const quint64 queryId = id.mid(5).toULongLong(&ok);
    if (!ok || m_pendingQueries.remove(queryId) == 0) {
        return;
    }
    qCWarning(verzetaRag) << "RagService: query embedding failed for" << id << error;
    // Deliver an empty result so the caller proceeds un-augmented (N4).
    emit retrievalReady(queryId, {});
}

QStringList RagService::computeVisibility(const QString& conversationId) const {
    QStringList scopes;
    scopes << QStringLiteral("global");
    if (conversationId.isEmpty()) {
        return scopes;
    }
    scopes << QStringLiteral("conversation:%1").arg(conversationId);
    // Add the project scope when the conversation belongs to a folder.
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT folder_id FROM conversations WHERE id = ?"));
    q.addBindValue(conversationId);
    if (q.exec() && q.next()) {
        const QString folderId = q.value(0).toString();
        if (!folderId.isEmpty()) {
            scopes << QStringLiteral("project:%1").arg(folderId);
        }
    }
    return scopes;
}

QList<RagChunk> RagService::recallWithVector(const QVector<float>& queryVec,
                                             const QString& model,
                                             const QString& conversationId,
                                             int topK) {
    // The query is already embedded by the coordinator; rank synchronously over
    // the conversation's visibility set. No embed, no network, no blocking.
    if (queryVec.isEmpty()) {
        return {};
    }
    return scoreScoped(queryVec, model, computeVisibility(conversationId), topK);
}

QList<RagChunk> RagService::scoreScoped(const QVector<float>& queryEmbedding,
                                        const QString& modelId,
                                        const QStringList& scopes,
                                        int topK) {
    QList<RagChunk> results;

    // Rank via the shared vector store (vec0 KNN with brute-force fallback). It
    // returns base rowids + cosine scores; we resolve those to the RAG chunk
    // payload (text + source) below, preserving the ranked order.
    const QList<VectorStore::Hit> hits = m_vectors.search(queryEmbedding, modelId, scopes, topK);
    if (hits.isEmpty()) {
        return results;
    }

    // Fetch the chunk payload for the ranked rowids in one query. SQL `IN` does
    // not preserve order, so we index by rowid and reassemble in hit order.
    QString placeholders;
    for (int i = 0; i < hits.size(); ++i) {
        placeholders += (i == 0) ? QStringLiteral("?") : QStringLiteral(",?");
    }
    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("SELECT rowid, chunk_text, source_id, source_type, embedding "
                             "FROM embeddings WHERE rowid IN (%1)")
                  .arg(placeholders));
    for (const VectorStore::Hit& h : hits) {
        q.addBindValue(h.rowid);
    }
    if (!q.exec()) {
        qCWarning(verzetaRag) << "RagService: scoreScoped payload query failed"
                              << q.lastError().text();
        return results;
    }
    QHash<qint64, RagChunk> byRow;
    while (q.next()) {
        RagChunk c;
        c.text = q.value(1).toString();
        c.sourceId = q.value(2).toString();
        c.sourceType = q.value(3).toString();
        // Carry the stored embedding so the unified retriever can dedup/rank this
        // chunk against candidates from the other memory sources (MMR).
        c.embedding = VectorStore::deserialize(q.value(4).toByteArray());
        byRow.insert(q.value(0).toLongLong(), c);
    }
    for (const VectorStore::Hit& h : hits) {
        const auto it = byRow.constFind(h.rowid);
        if (it == byRow.constEnd()) {
            continue;  // row removed between ranking and payload fetch
        }
        RagChunk c = it.value();
        c.score = h.score;
        results.append(c);
    }
    return results;
}

// ---------------------------------------------------------------------------
// Counts
// ---------------------------------------------------------------------------

/**
 * @brief Returns the number of indexed documents.
 */
int RagService::documentCount() const {
    QSqlQuery q(m_db.db());
    if (q.exec(QStringLiteral("SELECT COUNT(*) FROM documents")) && q.next()) {
        return q.value(0).toInt();
    }
    return 0;
}

/**
 * @brief Returns the total number of stored embedding chunks.
 */
int RagService::embeddingCount() const {
    QSqlQuery q(m_db.db());
    if (q.exec(QStringLiteral("SELECT COUNT(*) FROM embeddings")) && q.next()) {
        return q.value(0).toInt();
    }
    return 0;
}

int RagService::clearAllEmbeddings() {
    // VectorStore::clearAll() deletes the embeddings rows AND drops the vec0
    // index; documents + in-flight pending state are RAG-owned, cleared here.
    const int before = m_vectors.clearAll();
    QSqlQuery delDocs(m_db.db());
    delDocs.exec(QStringLiteral("DELETE FROM documents"));  // full reset

    m_pendingText.clear();
    m_pendingScope.clear();
    m_pendingChunks.clear();

    qCInfo(verzetaRag) << "RagService: cleared ALL embeddings (" << before << "rows) + documents";
    emit embeddingsCleared();
    return before;
}

int RagService::clearConversationEmbeddings(const QString& conversationId) {
    if (conversationId.isEmpty()) {
        return 0;
    }
    const QString scope = QStringLiteral("conversation:%1").arg(conversationId);
    // VectorStore::purgeScope() deletes the scoped rows and invalidates the vec0
    // index so it rebuilds from what remains.
    const int before = m_vectors.purgeScope(scope);
    qCInfo(verzetaRag) << "RagService: cleared" << before << "embeddings for" << scope;
    emit embeddingsCleared();
    return before;
}

// ---------------------------------------------------------------------------
// Private slots
// ---------------------------------------------------------------------------

/**
 * @brief Receives a computed embedding and persists it to the database.
 * @param id        Chunk tracking ID ("sourceId::chunkIndex::sourceType").
 * @param embedding Float32 vector.
 */
void RagService::onEmbeddingReady(const QString& id,
                                  const QVector<float>& embedding,
                                  const QString& modelId) {
    // Skip the synchronous query embedding and async-retrieval query embeds —
    // those are not corpus rows to store (handled by onQueryEmbeddingReady).
    if (id == QStringLiteral("__query__") || id.startsWith(QStringLiteral("ragq:"))) {
        return;
    }

    const QStringList parts = id.split(QStringLiteral("::"));
    if (parts.size() < 3) {
        return;
    }
    const QString sourceId = parts[0];
    const QString sourceType = parts[2];

    // Recover the chunk text remembered at scheduling time. Without it the
    // embeddings.chunk_text NOT NULL column cannot be populated, so skip
    // storing rather than fail the insert.
    const QString chunkText = m_pendingText.take(id);
    if (chunkText.isEmpty()) {
        m_pendingScope.remove(id);
        qCWarning(verzetaRag) << "RagService: no pending text for chunk" << id
                              << "— skipping store";
        return;
    }

    // Recover the visibility scope decided at index time; fall back to global
    // so a row is never stored with an empty (un-queryable) scope.
    QString ownerScope = m_pendingScope.take(id);
    if (ownerScope.isEmpty()) {
        ownerScope = QStringLiteral("global");
    }

    // Record the exact model that produced the vector so retrieval can filter
    // by model/dimension; never store an empty model identifier.
    const QString modelUsed = modelId.isEmpty() ? QStringLiteral("unknown") : modelId;

    const QByteArray blob = VectorStore::serialize(embedding);

    QSqlQuery q(m_db.db());
    q.prepare(QStringLiteral("INSERT OR REPLACE INTO embeddings"
                             "(id, source_id, source_type, chunk_text, embedding, model_used, "
                             "owner_scope, created_at) "
                             "VALUES(?, ?, ?, ?, ?, ?, ?, ?)"));
    q.addBindValue(id);
    q.addBindValue(sourceId);
    q.addBindValue(sourceType);
    q.addBindValue(chunkText);
    q.addBindValue(blob);
    q.addBindValue(modelUsed);
    q.addBindValue(ownerScope);
    q.addBindValue(QDateTime::currentMSecsSinceEpoch());

    if (!q.exec()) {
        qCWarning(verzetaRag) << "RagService: failed to store embedding" << id
                              << q.lastError().text();
    } else {
        // Mirror the new vector into the shared vec0 KNN index. upsert() is a
        // no-op when sqlite-vec is unavailable (brute-force then reads the BLOB
        // directly) and rebuilds the index on a dimension change.
        QSqlQuery rq(m_db.db());
        rq.prepare(QStringLiteral("SELECT rowid FROM embeddings WHERE id = ?"));
        rq.addBindValue(id);
        if (rq.exec() && rq.next()) {
            m_vectors.upsert(rq.value(0).toLongLong(), embedding, modelUsed, ownerScope);
        }
    }

    // Decrement pending counter and emit documentIndexed when all chunks done
    if (m_pendingChunks.contains(sourceId)) {
        int& pending = m_pendingChunks[sourceId];
        --pending;
        if (pending <= 0) {
            m_pendingChunks.remove(sourceId);
            emit documentIndexed(sourceId);
        }
    }
}

/**
 * @brief Handles embedding computation errors.
 */
void RagService::onEmbeddingError(const QString& id, const QString& error) {
    // Query embeds (sync "__query__" or async "ragq:<id>") are not corpus rows;
    // async-query failures are handled by onQueryEmbeddingError.
    if (id == QStringLiteral("__query__") || id.startsWith(QStringLiteral("ragq:"))) {
        return;
    }
    const QStringList parts = id.split(QStringLiteral("::"));
    const QString sourceId = parts.isEmpty() ? id : parts[0];
    qCWarning(verzetaRag) << "RagService: embedding error for" << id << error;
    emit indexingError(sourceId, error);
}

// ---------------------------------------------------------------------------
// Text chunking
// ---------------------------------------------------------------------------

/**
 * @brief Splits text into overlapping fixed-size chunks, preferring sentence ends.
 * @param text      Input text.
 * @param chunks    Output list.
 * @param chunkSize Target chunk size in characters.
 * @param overlap   Overlap between consecutive chunks.
 */
void RagService::chunkText(const QString& text, QStringList& chunks, int chunkSize, int overlap) {
    if (text.isEmpty()) {
        return;
    }

    int start = 0;
    while (start < text.length()) {
        int end = qMin(start + chunkSize, text.length());

        // Try to split on a sentence boundary (. ! ?) followed by whitespace
        if (end < text.length()) {
            // Look backwards from end for a sentence terminator
            int splitPos = end;
            for (int i = end - 1; i >= start + chunkSize / 2; --i) {
                const QChar c = text[i];
                if ((c == QLatin1Char('.') || c == QLatin1Char('!') || c == QLatin1Char('?')) &&
                    (i + 1 < text.length() && text[i + 1].isSpace())) {
                    splitPos = i + 1;
                    break;
                }
            }
            end = splitPos;
        }

        chunks.append(text.mid(start, end - start).trimmed());
        start = qMax(start + 1, end - overlap);
    }
}

// ---------------------------------------------------------------------------
// Embedding loading (vector (de)serialization lives in VectorStore)
// ---------------------------------------------------------------------------

/**
 * @brief Loads all embeddings from the database.
 */
QList<QPair<QString, QVector<float>>> RagService::loadEmbeddings() const {
    QList<QPair<QString, QVector<float>>> result;

    QSqlQuery q(m_db.db());
    if (!q.exec(
            QStringLiteral("SELECT id, embedding FROM embeddings WHERE embedding IS NOT NULL"))) {
        qCWarning(verzetaRag) << "RagService: loadEmbeddings query failed" << q.lastError().text();
        return result;
    }

    while (q.next()) {
        const QString embId = q.value(0).toString();
        const QByteArray blob = q.value(1).toByteArray();
        if (!blob.isEmpty()) {
            result.append({embId, VectorStore::deserialize(blob)});
        }
    }

    return result;
}

// Vector ranking, cosine similarity, JSON-array encoding, and the sqlite-vec
// KNN index all live in VectorStore (shared by every embedding consumer);
// RagService delegates to its m_vectors instance.
