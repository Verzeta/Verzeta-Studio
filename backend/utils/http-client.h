// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file http-client.h
 * @brief Wrapper around QNetworkAccessManager providing both standard
 *        and Server-Sent Events (SSE) streaming HTTP POST/GET requests.
 *        Implements exponential backoff retry on rate limit (HTTP 429).
 * @layer Utility
 * @dependencies Qt6::Network
 */

#pragma once

#include <QByteArray>
#include <QMap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QUrl>

/**
 * @brief HTTP client for LLM API communication with SSE streaming support.
 *
 * Features:
 * - Standard POST and GET requests (non-streaming)
 * - Server-Sent Events (SSE) streaming via postStream()
 * - Automatic NDJSON handling (Ollama)
 * - Retry with exponential backoff on HTTP 429 (rate limit): 1s, 2s, 4s (max 3 retries)
 * - 120-second timeout for streaming requests
 * - 30-second timeout for non-streaming requests
 *
 * Each HttpClient instance manages one active request. To handle multiple
 * concurrent requests, create multiple HttpClient instances.
 *
 * Security: API keys are passed via headers and never logged even at DEBUG level.
 */
class HttpClient : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Constructs the HttpClient with its own QNetworkAccessManager.
     * @param parent Optional Qt parent.
     */
    explicit HttpClient(QObject* parent = nullptr);

    /**
     * @brief Sends a non-streaming HTTP POST request.
     * @param url Target URL (must be HTTPS for remote providers).
     * @param body Request body bytes (JSON encoded).
     * @param headers HTTP request headers (e.g., {"Authorization": "Bearer sk-..."}).
     *                Headers are not logged.
     * @sideeffects Initiates network request.
     *              Emits responseReceived() on success, errorOccurred() on failure.
     */
    void post(const QUrl& url, const QByteArray& body, const QMap<QString, QString>& headers);

    /**
     * @brief Sends a streaming HTTP POST request (SSE or NDJSON).
     *        Emits chunkReceived() for each SSE "data:" event line or NDJSON line.
     * @param url Target URL.
     * @param body Request body bytes (JSON encoded).
     * @param headers HTTP request headers.
     * @sideeffects Initiates streaming network request.
     *              Emits chunkReceived() repeatedly, then streamFinished() on end.
     */
    void postStream(const QUrl& url, const QByteArray& body, const QMap<QString, QString>& headers);

    /**
     * @brief Sends a non-streaming HTTP GET request.
     * @param url Target URL.
     * @param headers Optional HTTP headers.
     * @sideeffects Initiates GET network request.
     *              Emits responseReceived() on success.
     */
    void get(const QUrl& url, const QMap<QString, QString>& headers = {});

    /**
     * @brief Cancels the currently in-progress request.
     * @sideeffects Calls QNetworkReply::abort(). Safe to call even if no request is active.
     */
    void cancel();

    /**
     * @brief Returns whether a request is currently in progress.
     * @return true if a network reply is active.
     */
    bool isActive() const;

  signals:
    /**
     * @brief Emitted when a non-streaming request completes.
     * @param statusCode HTTP status code (200, 401, 429, etc.).
     * @param body Response body bytes.
     */
    void responseReceived(int statusCode, const QByteArray& body);

    /**
     * @brief Emitted for each SSE "data:" event line or NDJSON line during streaming.
     * @param chunk Raw bytes of the data line (without the "data: " prefix for SSE).
     */
    void chunkReceived(const QByteArray& chunk);

    /**
     * @brief Emitted when a streaming request ends (connection closed or [DONE]).
     */
    void streamFinished();

    /**
     * @brief Emitted on network or protocol error.
     * @param message Human-readable error. Does not contain API keys or full request bodies.
     */
    void errorOccurred(const QString& message);

  private:
    QNetworkAccessManager m_nam;
    QNetworkReply* m_reply = nullptr;
    QByteArray m_sseBuffer;      ///< Accumulation buffer for incomplete SSE lines
    int m_retryCount = 0;        ///< Current retry attempt count (for 429 handling)
    bool m_isStreaming = false;  ///< Whether the current request is a streaming request

    /// A non-cancel error reported by errorOccurred() for the in-flight
    /// reply, stashed so onReplyFinished() (which fires AFTER
    /// errorOccurred() AND after the response body has fully arrived)
    /// can build the user-facing message from the complete body. At
    /// errorOccurred() time the body is not yet downloaded, so reading
    /// it there yields nothing. NoError = no error pending.
    QNetworkReply::NetworkError m_pendingError = QNetworkReply::NoError;
    /// Qt's errorString() for m_pendingError, captured at errorOccurred()
    /// time (the reply is still valid then). Used as the message for a
    /// pure transport failure that carries no HTTP body.
    QString m_pendingErrorString;

    // Stored for retry logic
    QUrl m_lastUrl;
    QByteArray m_lastBody;
    QMap<QString, QString> m_lastHeaders;

    /**
     * @brief Applies standard headers to a network request.
     * @param request The QNetworkRequest to modify.
     * @param headers Map of header name → value pairs.
     * @param streaming If true, sets Accept: text/event-stream.
     * @sideeffects Modifies the request in place.
     */
    void applyHeaders(QNetworkRequest& request,
                      const QMap<QString, QString>& headers,
                      bool streaming) const;

    /**
     * @brief Processes bytes available from a streaming reply.
     *        Buffers incomplete lines, emits complete SSE events or NDJSON lines.
     * @sideeffects Emits chunkReceived() for each complete data line.
     *              Handles "data: [DONE]" sentinel for OpenAI.
     */
    void onReadyRead();

    /**
     * @brief Handles reply completion.
     * @sideeffects Emits streamFinished(), handles retry on 429, emits errorOccurred().
     */
    void onReplyFinished();

    /**
     * @brief Handles reply network errors.
     * @param error The Qt network error code.
     * @sideeffects Emits errorOccurred().
     */
    void onReplyError(QNetworkReply::NetworkError error);

    /**
     * @brief Schedules a retry after an exponential backoff delay.
     * @param delayMs Delay in milliseconds.
     */
    void scheduleRetry(int delayMs);
};
