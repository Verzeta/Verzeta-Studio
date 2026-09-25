// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file rag-service.h
 * @brief Orchestrates the RAG (Retrieval-Augmented Generation) pipeline.
 *        Indexes documents and messages by chunking and embedding them, then
 *        retrieves the most relevant chunks for a query via cosine similarity.
 * @layer Service (Search Index)
 * @dependencies DbManager (Data Access), EmbeddingWorker (Worker)
 */


#pragma once

#include "models/db-manager.h"
#include "services/vector-store.h"
#include "workers/embedding-worker.h"

#include <QByteArray>
#include <QHash>
#include <QList>
#include <QMap>
#include <QObject>
#include <QString>
#include <QVector>

/**
 * @brief A single retrieved context chunk from the RAG corpus.
 */
struct RagChunk {
    QString sourceId;          ///< Originating message_id or document_id
    QString sourceType;        ///< "message" or "document"
    QString text;              ///< The chunk text content
    float score;               ///< Cosine similarity score in [−1, 1]; higher is more relevant
    QVector<float> embedding;  ///< The chunk's stored embedding (for cross-source
                               ///< dedup/MMR in the unified retriever); empty if N/A
};

/**
 * @brief RAG pipeline: document/message indexing, embedding storage, and retrieval.
 *
 * ## Pipeline overview
 * 1. **Index:** call indexDocument() or indexMessage() → text is chunked →
 *    EmbeddingWorker::computeEmbedding() emits embeddingReady →
 *    embedding stored in SQLite `embeddings` table.
 * 2. **Retrieve:** call retrieve(query) → query is embedded → brute-force cosine
 *    similarity over all stored embeddings → top-K chunks returned.
 * 3. **Augment:** call augmentPrompt() → retrieve() → prepend context to system prompt.
 *
 * RAG enable is PER CONVERSATION (llm_config.rag_enabled), decided by the
 * callers; this service is a stateless engine with no global enable flag.
 */
class RagService : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Constructs a RagService.
     * @param db      Reference to the open database.
     * @param embedder Reference to the EmbeddingWorker (may be on another thread).
     * @param parent  Optional Qt parent.
     */
    explicit RagService(DbManager& db, EmbeddingWorker& embedder, QObject* parent = nullptr);

    /**
     * @brief Constructs a RagService with SEPARATE embedding workers for bulk
     *        indexing vs. live query retrieval.
     *
     * Routing index embeds (computeEmbedding for storage) to a dedicated
     * bulk worker and query embeds (retrieveAsync/retrieveAllAsync) to a
     * priority worker means a live recall never queues behind a backlog of
     * background indexing jobs in busy concurrent group chats.
     * @param db           Reference to the open database.
     * @param bulkEmbedder  Worker for indexing/storage embeds.
     * @param queryEmbedder Worker for live query/retrieval embeds.
     * @param parent        Optional Qt parent.
     */
    explicit RagService(DbManager& db,
                        EmbeddingWorker& bulkEmbedder,
                        EmbeddingWorker& queryEmbedder,
                        QObject* parent = nullptr);
    ~RagService() override;

    // -----------------------------------------------------------------------
    // Indexing
    // -----------------------------------------------------------------------

    /**
     * @brief Indexes a document by chunking and computing embeddings.
     * @param docPath Absolute file path (used as source ID).
     * @param content Full text content of the document.
     * @param ownerScope Visibility scope stored on each chunk
     *                   (`global` | `conversation:\<id\>` | `project:\<folderId\>` |
     *                   `agent:\<id\>`). Knowledge-base documents added from
     *                   settings are `global`; defaults to `global`.
     * @sideeffects Inserts a row into `documents` table and stores each chunk's
     *              embedding in `embeddings` table. Async: embedding computation
     *              happens in EmbeddingWorker; documentIndexed() is emitted on completion.
     */
    void indexDocument(const QString& docPath,
                       const QString& content,
                       const QString& ownerScope = QStringLiteral("global"));

    /**
     * @brief Reads a file from disk and indexes it into the RAG corpus.
     * @param filePath Absolute path (or file:// URL) to a UTF-8 text file.
     * @returns true if the file was read and indexing was scheduled; false if
     *          the file could not be opened/read or RAG is disabled.
     * @sideeffects Same as indexDocument(). QML-invokable so the settings UI
     *              can add documents via a file picker.
     */
    Q_INVOKABLE bool indexDocumentFile(const QString& filePath);

    /**
     * @brief Indexes a single conversation message.
     * @param messageId UUID of the message.
     * @param text      Message text content.
     * @param ownerScope Visibility scope stored on the chunk, typically
     *                   `conversation:<convId>` so retrieval can scope to the
     *                   originating conversation (and its project/global).
     * @sideeffects Stores the embedding in `embeddings` table.
     *              Async: embedding computed via EmbeddingWorker.
     */
    void indexMessage(const QString& messageId, const QString& text, const QString& ownerScope);

    // -----------------------------------------------------------------------
    // Retrieval
    // -----------------------------------------------------------------------

    /**
     * @brief Retrieves the top-K most relevant chunks for a query.
     * @param query Search query text.
     * @param topK  Number of results to return (default 5).
     * @return List of RagChunk sorted by descending cosine similarity.
     * @sideeffects Computes a query embedding synchronously.
     *              Loads all stored embeddings from SQLite for comparison.
     * @complexity O(n*d) brute-force over stored embeddings.
     *
     * Returns empty list if RAG is disabled or no embeddings are stored.
     */
    QList<RagChunk> retrieve(const QString& query, int topK = 5);

    /**
     * @brief Augments a prompt with retrieved context chunks.
     * @param userQuery  The user's current query text.
     * @param basePrompt The base system prompt to augment.
     * @param topK       Number of context chunks to include (default 5).
     * @return Augmented prompt string:
     *   "{basePrompt}\n\n## Relevant Context\n{chunk1}\n---\n{chunk2}\n---\n..."
     *
     * Returns basePrompt unchanged if RAG is disabled or no relevant chunks found.
     */
    QString augmentPrompt(const QString& userQuery, const QString& basePrompt, int topK = 5);

    /**
     * @brief Asynchronously retrieves the top-K relevant chunks for a query,
     *        scoped to a conversation's visibility set. Never blocks the caller.
     *
     * The query embedding is computed on the embedding worker thread; when it
     * arrives, scope- and model-filtered cosine ranking runs and the result is
     * delivered via the retrievalReady() signal. This is the turn-path entry
     * point that replaces the synchronous retrieve()/augmentPrompt blocking on
     * the main thread.
     * @param query          The query text to embed and match.
     * @param conversationId The originating conversation; its visibility set
     *                       ({global} ∪ {conversation:\<id\>} ∪ {project:\<folderId\>}
     *                       when the conversation is in a folder) bounds results.
     * @param topK           Maximum number of chunks to return.
     * @returns A non-zero query id used to correlate the retrievalReady() signal,
     *          or 0 if no retrieval was started (RAG disabled or empty query),
     *          in which case no signal is emitted and the caller proceeds
     *          un-augmented.
     */
    quint64 retrieveAsync(const QString& query, const QString& conversationId, int topK = 5);

    /**
     * @brief Asynchronously retrieves the top-K relevant chunks across the
     *        ENTIRE corpus (no scope filter) for cross-conversation search.
     *
     * Same async, signal-driven contract as retrieveAsync() (delivers via
     * retrievalReady), but unscoped: search is meant to find anything, anywhere.
     * Still model-filtered so dimensions match.
     * @param query The query text to embed and match.
     * @param topK  Maximum number of chunks to return.
     * @returns A non-zero query id correlating the retrievalReady() signal, or 0
     *          if no retrieval was started (RAG disabled or empty query).
     */
    quint64 retrieveAllAsync(const QString& query, int topK = 5);

    /**
     * @brief Synchronously ranks the RAG corpus against an ALREADY-COMPUTED
     *        query embedding, scoped to a conversation's visibility set.
     *
     * For the unified pre-turn retrieval coordinator (MemoryRetriever), which
     * embeds the query ONCE and fans the vector out to every enabled source,
     * so RAG does NOT embed here. Pure DB ranking (vec0 KNN or brute-force
     * fallback via VectorStore) + a payload fetch; fast, main-thread, no
     * network. With no stored vectors of the query's model it returns empty.
     * @param queryVec       The query embedding (already computed).
     * @param model          The model that produced it (dimension/model filter).
     * @param conversationId The conversation whose visibility set bounds results
     *                       ({global} ∪ {conversation:} ∪ {project:}).
     * @param topK           Maximum chunks to return.
     * @returns Ranked chunks (highest cosine first), possibly empty.
     */
    QList<RagChunk> recallWithVector(const QVector<float>& queryVec,
                                     const QString& model,
                                     const QString& conversationId,
                                     int topK);

    /**
     * @brief Formats retrieved chunks into the system-prompt context block.
     * @param chunks Ranked chunks (e.g. from a retrievalReady signal).
     * @returns The "\\n\\n## Relevant Context\\n…" block to append to a system
     *          prompt, or an empty string when chunks is empty. Pure function and
     *          the single source of truth for the augmentation format, shared by
     *          augmentPrompt() and the async turn path.
     */
    static QString formatChunksAsContext(const QList<RagChunk>& chunks);

    // -----------------------------------------------------------------------
    // Properties
    // -----------------------------------------------------------------------

    /**
     * @brief Number of indexed documents.
     * @returns Document count.
     * @complexity O(1): SELECT COUNT(*).
     */
    Q_INVOKABLE int documentCount() const;

    /**
     * @brief Total number of embedding chunks stored.
     * @returns Chunk count.
     * @complexity O(1): SELECT COUNT(*).
     */
    Q_INVOKABLE int embeddingCount() const;

    /**
     * @brief Deletes all indexed embeddings AND documents for a full corpus reset
     *        ("start fresh"). Also drops the vec0 KNN index. Indexing resumes
     *        forward as new messages arrive (RAG stays as configured).
     * @returns The number of embedding rows deleted.
     */
    Q_INVOKABLE int clearAllEmbeddings();

    /**
     * @brief Deletes the embeddings indexed for one conversation
     *        (owner_scope == "conversation:\<id\>"), leaving other conversations
     *        and global documents intact. Invalidates the vec0 index so it
     *        rebuilds from what remains.
     * @param conversationId The conversation whose embeddings to clear.
     * @returns The number of embedding rows deleted.
     */
    Q_INVOKABLE int clearConversationEmbeddings(const QString& conversationId);

  signals:
    /** @brief Emitted when a document has been fully indexed (all chunks stored). */
    void documentIndexed(const QString& docId);

    /** @brief Emitted after embeddings are cleared, so UIs can refresh counts. */
    void embeddingsCleared();

    /**
     * @brief Emitted when indexing fails for a document or message.
     * @param sourceId Document / message id whose indexing failed.
     * @param error    Human-readable error description.
     */
    void indexingError(const QString& sourceId, const QString& error);

    /**
     * @brief Emitted when an async retrieval (retrieveAsync) completes.
     * @param queryId The id returned by the originating retrieveAsync() call.
     * @param chunks  The ranked chunks (possibly empty on miss or embed failure;
     *                an empty result is delivered so the caller can proceed
     *                un-augmented rather than stall).
     */
    void retrievalReady(quint64 queryId, const QList<RagChunk>& chunks);

  private slots:
    /**
     * @brief Receives a computed embedding from EmbeddingWorker and stores it in DB.
     * @param id        Embedding chunk ID (format: "sourceId::chunkIndex::sourceType").
     * @param embedding Float32 embedding vector.
     * @param modelId   Identifier of the model that produced the vector; stored
     *                  as `model_used` so retrieval can filter by model/dimension.
     */
    void
    onEmbeddingReady(const QString& id, const QVector<float>& embedding, const QString& modelId);

    /**
     * @brief Handles embedding computation errors.
     * @param id    Chunk ID.
     * @param error Error description.
     */
    void onEmbeddingError(const QString& id, const QString& error);

    /**
     * @brief Receives a query embedding for an async retrieval and emits
     *        retrievalReady with the scope/model-filtered ranked chunks.
     * @param id        Worker tracking id ("ragq:<queryId>"); ignored if it is
     *                  not a pending query id.
     * @param embedding The query embedding vector.
     * @param modelId   The model that produced the query vector; results are
     *                  restricted to stored chunks from the same model so
     *                  dimensions always match.
     */
    void onQueryEmbeddingReady(const QString& id,
                               const QVector<float>& embedding,
                               const QString& modelId);

    /**
     * @brief Handles a failed query embedding for an async retrieval by
     *        delivering an empty retrievalReady (caller proceeds un-augmented).
     * @param id    Worker tracking id ("ragq:<queryId>"); ignored if not a
     *              pending query id.
     * @param error Error description (logged).
     */
    void onQueryEmbeddingError(const QString& id, const QString& error);

  private:
    DbManager& m_db;
    // Bulk worker handles indexing/storage embeds; query worker handles live
    // retrieval embeds (G4). They are the SAME instance when constructed via the
    // single-worker constructor (no separation), or distinct instances via the
    // bulk/query constructor.
    EmbeddingWorker& m_bulkEmbedder;
    EmbeddingWorker& m_queryEmbedder;

    // Shared embedding vector store over the RAG corpus tables
    // (embeddings + vec_embeddings): float32 (de)serialization, cosine ranking,
    // and the sqlite-vec KNN index with brute-force fallback. The same class
    // backs other embedding consumers (e.g. agent memory) so the vector
    // mechanics live in exactly one place.
    VectorStore m_vectors;

    /**
     * @brief Wires the bulk worker's signals to the storage slots and the query
     *        worker's signals to the query slots. Shared by both constructors.
     *        When bulk == query, both slot sets bind to the one worker and the
     *        id-prefix guards route correctly.
     */
    void wireEmbedderSignals();

    // Track pending chunks per document to know when all chunks are stored
    QMap<QString, int> m_pendingChunks;  ///< sourceId → remaining pending chunks

    // Carry each chunk's text from scheduling (indexDocument/indexMessage) to
    // storage (onEmbeddingReady), keyed by the chunk tracking id. The worker
    // only returns (id, vector); the text must be remembered here so the
    // embeddings.chunk_text NOT NULL column can be populated.
    QHash<QString, QString> m_pendingText;  ///< chunkId → chunk text awaiting store

    // Carry each chunk's owner_scope from scheduling to storage, keyed by the
    // same chunk tracking id. The worker returns only (id, vector, modelId);
    // the visibility scope decided at index time is remembered here so the
    // embeddings.owner_scope column is populated correctly.
    QHash<QString, QString> m_pendingScope;  ///< chunkId → owner_scope awaiting store

    // Async retrieval (retrieveAsync) bookkeeping. Each in-flight query gets a
    // monotonic id; its parameters are held until the worker returns the query
    // embedding, at which point scope/model-filtered ranking runs and
    // retrievalReady is emitted. Signal-driven — no timers, no blocking.
    /**
     * @brief In-flight async-retrieval parameters, held until the query
     *        embedding returns (the owner_scope set to match against + the
     *        result cap).
     */
    struct PendingQuery {
        QStringList scopes;  ///< owner_scope visibility set to match against
        int topK = 5;
    };
    QHash<quint64, PendingQuery> m_pendingQueries;  ///< queryId → params in flight
    quint64 m_nextQueryId = 0;                      ///< monotonic query id source

    /**
     * @brief Computes the owner_scope visibility set for a conversation:
     *        {global} ∪ {conversation:\<id\>} ∪ {project:\<folderId\>} when the
     *        conversation belongs to a folder.
     * @param conversationId The conversation whose visibility is resolved.
     * @returns The list of owner_scope values a turn in this conversation may see.
     */
    QStringList computeVisibility(const QString& conversationId) const;

    /**
     * @brief Ranks stored chunks against a query embedding, restricted to the
     *        given scopes and to the query's own model (so dimensions match).
     * @param queryEmbedding The query vector.
     * @param modelId        Only chunks stored with this model are compared.
     * @param scopes         owner_scope visibility set to restrict to.
     * @param topK           Maximum number of chunks to return.
     * @returns Ranked RagChunk list (highest cosine first), possibly empty.
     * @complexity O(n*d) over the scope/model-filtered subset
     *             (brute-force comparison, no vector index).
     */
    QList<RagChunk> scoreScoped(const QVector<float>& queryEmbedding,
                                const QString& modelId,
                                const QStringList& scopes,
                                int topK);

    // ---------------------------------------------------------------------------
    // Text chunking
    // ---------------------------------------------------------------------------

    /**
     * @brief Splits text into overlapping chunks, preferring sentence boundaries.
     * @param text      Input text.
     * @param chunks    Output: list of chunk strings.
     * @param chunkSize Target chunk size in characters (approximate tokens × 4).
     * @param overlap   Overlap between consecutive chunks in characters.
     * @complexity O(n) where n is text length.
     */
    void chunkText(const QString& text, QStringList& chunks, int chunkSize = 512, int overlap = 64);

    /**
     * @brief Strips @-mention sigils from message text before indexing
     *        ("@Bob" → "Bob"), so retrieved/injected group-chat context does
     *        not prime the model to echo routing tags (e.g. tag itself).
     * @param text The raw message text.
     * @returns The text with @-mention sigils removed (names preserved).
     */
    static QString stripMentions(const QString& text);

    // ---------------------------------------------------------------------------
    // Embedding storage / retrieval
    // ---------------------------------------------------------------------------

    /**
     * @brief Loads all embeddings from the database.
     * @return List of (embeddingId, vector) pairs.
     * @complexity O(n*d) where n is embedding count.
     */
    QList<QPair<QString, QVector<float>>> loadEmbeddings() const;
};
