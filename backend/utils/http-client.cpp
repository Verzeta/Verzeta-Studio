// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file http-client.cpp
 * @brief Implementation of the HTTP client with SSE streaming support,
 *        timeout management, and exponential backoff retry on rate limiting.
 * @layer Utility
 * @dependencies Qt6::Network
 */

#include "http-client.h"

#include "logger.h"

#include <QTimer>

#include <QNetworkRequest>
#include <QSslConfiguration>

// Timeout constants
static constexpr int kStreamingTimeoutMs = 120000;    ///< 120 seconds for streaming
static constexpr int kNonStreamingTimeoutMs = 30000;  ///< 30 seconds for standard requests
static constexpr int kMaxRetries = 3;                 ///< Max retries on HTTP 429
static constexpr int kBaseRetryDelayMs = 1000;        ///< Initial retry delay (doubles each time)

/// SSE end-of-stream sentinel sent by OpenAI-style servers.
static const QByteArray kSseDoneMarker = QByteArrayLiteral("[DONE]");

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

/**
 * @brief Constructs HttpClient with its own QNetworkAccessManager.
 * @param parent Optional Qt parent.
 */
HttpClient::HttpClient(QObject* parent) : QObject(parent) {
    // Transfer encoding must be handled manually for streaming
    m_nam.setAutoDeleteReplies(true);
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/*
 * @brief Sends a non-streaming HTTP POST request.
 * @param url Target URL.
 * @param body JSON body bytes.
 * @param headers Request headers.
 * @sideeffects Initiates network request.
 */
void HttpClient::post(const QUrl& url,
                      const QByteArray& body,
                      const QMap<QString, QString>& headers) {
    cancel();  // Cancel any in-progress request

    m_lastUrl = url;
    m_lastBody = body;
    m_lastHeaders = headers;
    m_isStreaming = false;
    m_retryCount = 0;

    QNetworkRequest req(url);
    applyHeaders(req, headers, false);
    req.setTransferTimeout(kNonStreamingTimeoutMs);

    m_reply = m_nam.post(req, body);
    m_reply->ignoreSslErrors();  // Accept self-signed certs for local providers
    connect(m_reply, &QNetworkReply::finished, this, &HttpClient::onReplyFinished);
    connect(m_reply, &QNetworkReply::errorOccurred, this, &HttpClient::onReplyError);
}

/*
 * @brief Sends a streaming HTTP POST request (SSE/NDJSON).
 * @param url Target URL.
 * @param body JSON body bytes.
 * @param headers Request headers.
 * @sideeffects Initiates streaming network request.
 */
void HttpClient::postStream(const QUrl& url,
                            const QByteArray& body,
                            const QMap<QString, QString>& headers) {
    cancel();

    m_lastUrl = url;
    m_lastBody = body;
    m_lastHeaders = headers;
    m_isStreaming = true;
    m_retryCount = 0;
    m_sseBuffer.clear();

    QNetworkRequest req(url);
    applyHeaders(req, headers, true);
    req.setTransferTimeout(kStreamingTimeoutMs);

    m_reply = m_nam.post(req, body);
    m_reply->ignoreSslErrors();  // Accept self-signed certs for local providers
    connect(m_reply, &QNetworkReply::readyRead, this, &HttpClient::onReadyRead);
    connect(m_reply, &QNetworkReply::finished, this, &HttpClient::onReplyFinished);
    connect(m_reply, &QNetworkReply::errorOccurred, this, &HttpClient::onReplyError);
}

/*
 * @brief Sends a non-streaming HTTP GET request.
 * @param url Target URL.
 * @param headers Optional request headers.
 */
void HttpClient::get(const QUrl& url, const QMap<QString, QString>& headers) {
    cancel();

    m_isStreaming = false;
    m_retryCount = 0;

    QNetworkRequest req(url);
    applyHeaders(req, headers, false);
    req.setTransferTimeout(kNonStreamingTimeoutMs);

    m_reply = m_nam.get(req);
    m_reply->ignoreSslErrors();  // Accept self-signed certs for local providers
    connect(m_reply, &QNetworkReply::finished, this, &HttpClient::onReplyFinished);
    connect(m_reply, &QNetworkReply::errorOccurred, this, &HttpClient::onReplyError);
}

/**
 * @brief Cancels the in-progress request.
 * @sideeffects Aborts QNetworkReply if active.
 */
void HttpClient::cancel() {
    if (m_reply) {
        // CRITICAL: disconnect all signals from the dying reply BEFORE
        // calling abort(). QNetworkReply::abort() schedules its
        // errorOccurred/finished signals via QueuedConnection (Qt 6 default
        // for QNetworkReply), so they fire on the next event loop tick —
        // by which point a NEW reply may have been started and assigned
        // to m_reply. If the old reply's queued signals were still
        // connected, our slots would treat them as the NEW reply's
        // finish/error and trigger the validator on a fresh request that
        // has not done anything yet. Symptom: a brand-new task executor
        // turn appears to be cancelled in 1–4 ms, the text-only validator
        // marks it a failure, the step blocks. The reply object itself is
        // owned by QNAM (setAutoDeleteReplies(true)), so disconnecting and
        // abandoning is safe — Qt will tear it down later.
        QNetworkReply* dying = m_reply;
        m_reply = nullptr;
        disconnect(dying, nullptr, this, nullptr);
        dying->abort();
    }
    m_sseBuffer.clear();
    // Clear any error stashed for a previous reply so it cannot leak
    // into the next request (post/postStream/get all call cancel()
    // first, so this also covers the 429-retry path).
    m_pendingError = QNetworkReply::NoError;
    m_pendingErrorString.clear();
}

/**
 * @brief Returns whether a request is active.
 * @return true if a reply is active and not finished.
 */
bool HttpClient::isActive() const {
    return m_reply && !m_reply->isFinished();
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

/**
 * @brief Applies headers to a QNetworkRequest.
 * @param request The request to modify.
 * @param headers Map of header name → value.
 * @param streaming If true, sets Accept: text/event-stream.
 */
void HttpClient::applyHeaders(QNetworkRequest& request,
                              const QMap<QString, QString>& headers,
                              bool streaming) const {
    request.setHeader(QNetworkRequest::ContentTypeHeader, QByteArrayLiteral("application/json"));

    if (streaming) {
        request.setRawHeader(QByteArrayLiteral("Accept"), QByteArrayLiteral("text/event-stream"));
        request.setAttribute(QNetworkRequest::CacheLoadControlAttribute,
                             QNetworkRequest::AlwaysNetwork);
    }

    for (auto it = headers.begin(); it != headers.end(); ++it) {
        request.setRawHeader(it.key().toUtf8(), it.value().toUtf8());
    }
}

/**
 * @brief Processes available streaming data.
 *        Buffers incomplete lines, emits complete SSE events or NDJSON lines.
 * @sideeffects Emits chunkReceived() for each complete data line.
 */
void HttpClient::onReadyRead() {
    if (!m_reply) {
        return;
    }

    m_sseBuffer.append(m_reply->readAll());

    // If the server returned an HTTP error status, the bytes arriving
    // here are the error BODY (a JSON error object), not an SSE/NDJSON
    // stream. Do not parse or emit them as chunks — just accumulate;
    // onReplyFinished() reads m_sseBuffer to build the error message.
    // The status-code attribute is populated once response headers are
    // in, which is before the first readyRead().
    if (m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() >= 400) {
        return;
    }

    // Process complete lines separated by \n (NDJSON) or \n\n (SSE)
    while (true) {
        const int newlineIdx = m_sseBuffer.indexOf('\n');
        if (newlineIdx < 0) {
            break;  // Wait for more data
        }

        const QByteArray line = m_sseBuffer.left(newlineIdx).trimmed();
        m_sseBuffer.remove(0, newlineIdx + 1);

        if (line.isEmpty()) {
            continue;  // Blank separator line in SSE
        }

        // SSE "data:" prefix stripping
        if (line.startsWith(QByteArrayLiteral("data: "))) {
            const QByteArray data = line.mid(6);  // Skip "data: "
            if (data == kSseDoneMarker) {
                // OpenAI "[DONE]" sentinel — stream complete
                qCDebug(verzetaLlm) << "SSE [DONE] received";
                emit streamFinished();
                return;
            }
            emit chunkReceived(data);
        } else if (!line.startsWith(QByteArrayLiteral("event:")) &&
                   !line.startsWith(QByteArrayLiteral(":"))) {
            // Plain NDJSON line (Ollama) — emit directly
            emit chunkReceived(line);
        }
        // Lines starting with "event:" or ":" (SSE comment) are consumed without emitting
    }
}

/**
 * @brief Handles reply completion.
 *        Retries on HTTP 429, emits responseReceived or streamFinished on success.
 */
void HttpClient::onReplyFinished() {
    if (!m_reply) {
        return;
    }

    const int statusCode = m_reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();

    // Rate limit retry logic
    if (statusCode == 429 && m_retryCount < kMaxRetries) {
        ++m_retryCount;
        const int delay = kBaseRetryDelayMs * (1 << (m_retryCount - 1));  // 1s, 2s, 4s
        qCWarning(verzetaLlm) << "HTTP 429 rate limit — retry" << m_retryCount << "in" << delay
                              << "ms";
        m_reply = nullptr;
        m_pendingError = QNetworkReply::NoError;  // consumed by the retry
        m_pendingErrorString.clear();
        scheduleRetry(delay);
        return;
    }

    // Error path — handled HERE, not in onReplyError(), because:
    //   (a) finished() always fires AFTER errorOccurred(), and only by
    //       finished() is the response body fully downloaded — reading
    //       it in onReplyError() yields an empty body and the message
    //       degrades to Qt's opaque "...status code 400";
    //   (b) an HTTP error status with no transport error (errorOccurred
    //       not emitted) is still caught via statusCode >= 400.
    // For an HTTP 4xx/5xx the provider returns a JSON error explaining
    // EXACTLY what was wrong (unknown model, malformed field, bad key,
    // quota exceeded, ...); surfacing it is rule 11 — no silent or
    // unexplained failures.
    const bool hadTransportError = (m_pendingError != QNetworkReply::NoError);
    const bool httpError = (statusCode >= 400);
    if (hadTransportError || httpError) {
        // For a streaming reply, onReadyRead() may already have drained
        // the error body into m_sseBuffer; for a non-streaming reply it
        // is still on the reply. Combine both so we never miss it.
        QByteArray body = m_sseBuffer;
        body.append(m_reply->readAll());
        m_sseBuffer.clear();

        QString msg;
        if (!body.isEmpty()) {
            QString bodyText = QString::fromUtf8(body).simplified();
            // Cap the length so a stray HTML error page can't flood the
            // log or the UI error banner.
            constexpr int kMaxBodyChars = 600;
            if (bodyText.size() > kMaxBodyChars) {
                bodyText = bodyText.left(kMaxBodyChars) + QStringLiteral("… (truncated)");
            }
            msg = statusCode > 0 ? QStringLiteral("HTTP %1: %2").arg(statusCode).arg(bodyText)
                                 : bodyText;
        } else if (statusCode > 0) {
            msg = QStringLiteral("HTTP %1").arg(statusCode);
        } else {
            // Pure transport failure (connection refused, DNS, timeout)
            // — no HTTP response at all. Use Qt's errorString captured
            // at errorOccurred() time.
            msg = !m_pendingErrorString.isEmpty() ? m_pendingErrorString
                                                  : QStringLiteral("Network error");
        }

        qCWarning(verzetaLlm) << "Request failed:" << msg;
        m_pendingError = QNetworkReply::NoError;
        m_pendingErrorString.clear();
        m_reply = nullptr;
        emit errorOccurred(msg);
        return;
    }

    if (m_isStreaming) {
        // Process any remaining buffered data
        if (!m_sseBuffer.isEmpty()) {
            const QByteArray remaining = m_sseBuffer.trimmed();
            if (!remaining.isEmpty() && remaining != kSseDoneMarker) {
                emit chunkReceived(remaining);
            }
            m_sseBuffer.clear();
        }
        emit streamFinished();
    } else {
        const QByteArray body = m_reply->readAll();
        emit responseReceived(statusCode, body);
    }

    m_reply = nullptr;
}

/**
 * @brief Handles reply errors. A cancellation is logged and ignored; any
 *        other error is STASHED (not emitted here) so onReplyFinished()
 *        (which fires afterwards, once the response body has fully
 *        arrived) can build the user-facing message from the complete
 *        body. See onReplyFinished() for why the body cannot be read at
 *        this point.
 * @param error Qt network error code.
 */
void HttpClient::onReplyError(QNetworkReply::NetworkError error) {
    if (error == QNetworkReply::OperationCanceledError) {
        qCDebug(verzetaLlm) << "Request cancelled (OperationCanceledError)";
        return;
    }

    // Stash; onReplyFinished() builds and emits the message. errorString()
    // is captured now because it is the only meaningful description for a
    // pure transport failure (no HTTP body), and the reply is still valid
    // at this point.
    m_pendingError = error;
    m_pendingErrorString = m_reply ? m_reply->errorString() : QString();
}

/**
 * @brief Schedules a retry after a delay using QTimer::singleShot.
 * @param delayMs Delay in milliseconds.
 */
void HttpClient::scheduleRetry(int delayMs) {
    QTimer::singleShot(delayMs, this, [this]() {
        if (m_isStreaming) {
            postStream(m_lastUrl, m_lastBody, m_lastHeaders);
        } else {
            post(m_lastUrl, m_lastBody, m_lastHeaders);
        }
    });
}
