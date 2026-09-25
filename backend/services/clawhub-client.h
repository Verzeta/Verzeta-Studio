// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file clawhub-client.h
 * @brief HTTP client for the ClawHub skill catalog. QNetworkAccessManager
 *        only; HTTPS-only; max 3 redirects; 30s / 60s timeouts. No CLI
 *        shell-out.
 *
 *        Endpoints:
 *           GET /api/v1/search?q=&limit=&cursor=
 *           GET /api/v1/skills/\<slug\>
 *           GET /api/v1/download?slug=&version=    (binary ZIP stream)
 *
 *        Response-shape variants are tolerated (results | skills |
 *        items; nextCursor | next_cursor | cursor) per the reference
 *        Python script.
 *
 * @layer Service
 * @dependencies Qt6::Network
 */
#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

class QNetworkAccessManager;
class QNetworkReply;

/**
 * @brief Network client for the ClawHub skill catalog. Exposes search
 *        + per-slug enrichment + download endpoints to QML.
 */
class ClawHubClient : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool searchInFlight READ searchInFlight NOTIFY searchInFlightChanged)
    Q_PROPERTY(bool downloadInFlight READ downloadInFlight NOTIFY downloadInFlightChanged)

  public:
    /**
     * @brief Construct the client. Creates an owned
     *        QNetworkAccessManager and sets the default API base
     *        (https://clawhub.ai/api/v1).
     * @param parent Qt parent.
     */
    explicit ClawHubClient(QObject* parent = nullptr);
    ~ClawHubClient() override;

    /**
     * @brief Whether a search request is currently in flight.
     * @returns true while a search is awaiting a response.
     */
    bool searchInFlight() const { return m_searchInFlight; }

    /**
     * @brief Whether a download is currently in flight.
     * @returns true while a ZIP download is streaming.
     */
    bool downloadInFlight() const { return m_downloadInFlight; }

    /**
     * @brief Issue a paginated search. Emits `searchCompleted` on
     *        success or `searchFailed` on TLS / HTTP / parse error.
     * @param query  User query string (sent verbatim as the `q` param).
     * @param cursor Optional opaque cursor returned by a previous
     *               search call; empty for the first page.
     * @param limit  Max rows per page (server may clamp).
     */
    Q_INVOKABLE void search(const QString& query, const QString& cursor = {}, int limit = 10);

    /**
     * @brief Resolve detail (latestVersion etc.) for a slug, then
     *        download the matching ZIP archive. On success emits
     *        `downloadCompleted(slug, version, localZipPath)`; on
     *        failure emits `downloadFailed(slug, error)`.
     * @param slug Skill slug to fetch + download.
     */
    Q_INVOKABLE void downloadLatest(const QString& slug);

    /**
     * @brief Cancel any in-flight download. No-op when none is active.
     */
    Q_INVOKABLE void cancelDownload();

    /**
     * @brief Override the API root (test seam: unit tests with
     *        QHttpServer point at localhost). Defaults to
     *        https://clawhub.ai/api/v1.
     * @param base New API base URL.
     */
    void setApiBaseForTesting(const QString& base);

  signals:
    /** @brief Emitted when searchInFlight() changes value. */
    void searchInFlightChanged();

    /** @brief Emitted when downloadInFlight() changes value. */
    void downloadInFlightChanged();

    /**
     * @brief A paginated search returned successfully.
     * @param results    Parsed list of minimal skill rows.
     * @param nextCursor Opaque cursor for the next page (empty when
     *                   no more pages).
     */
    void searchCompleted(QVariantList results, QString nextCursor);

    /**
     * @brief A search request failed (TLS / HTTP / parse).
     * @param error Human-readable error description.
     */
    void searchFailed(QString error);

    /**
     * @brief Per-slug enrichment delivered AFTER the initial
     *        searchCompleted. The /search endpoint returns minimal
     *        rows (slug, displayName, summary), while /skills/\<slug\>
     *        carries the full envelope (owner, stats, moderation,
     *        latestVersion). After every search the client fires a
     *        detail request per slug and re-emits the merged row
     *        through this signal so QML can patch the search list.
     * @param slug Skill slug whose enrichment row is being delivered.
     * @param row  Full enriched row map.
     */
    void searchResultEnriched(QString slug, QVariantMap row);

    /**
     * @brief A download finished successfully.
     * @param slug          Skill slug that was downloaded.
     * @param version       Version of the downloaded ZIP.
     * @param localZipPath  Absolute path to the saved ZIP archive.
     */
    void downloadCompleted(QString slug, QString version, QString localZipPath);

    /**
     * @brief A download failed.
     * @param slug  Skill slug whose download failed.
     * @param error Human-readable error description.
     */
    void downloadFailed(QString slug, QString error);

  private slots:
    /** @brief Handle the search QNetworkReply::finished signal. */
    void onSearchFinished();
    /** @brief Handle the per-slug detail QNetworkReply::finished signal. */
    void onDetailFinished();
    /** @brief Handle the download QNetworkReply::finished signal. */
    void onDownloadFinished();
    /** @brief Stream incoming download bytes to disk on readyRead. */
    void onDownloadReadyRead();

  private:
    QString safeApiBase() const;
    void setSearchInFlight(bool v);
    void setDownloadInFlight(bool v);
    bool tlsAndHttpsCheck(QNetworkReply* reply, QString& outError);

    /** Parse a `/skills/\<slug\>` envelope (or a wrapped search row)
     *  into the same QVariantMap shape `searchCompleted` ships. Used
     *  by both the initial search-result loop and the per-slug
     *  enrichment slot. */
    static QVariantMap parseSkillEnvelope(const QJsonObject& envelope);

    /** Fire a detail request for `slug` and emit
     *  `searchResultEnriched` on success. Errors are silently ignored;
     *  the un-enriched row stays in the search list as-is. */
    void requestEnrichment(const QString& slug);

    QNetworkAccessManager* m_nam = nullptr;
    QString m_apiBase;
    bool m_searchInFlight = false;
    bool m_downloadInFlight = false;

    QNetworkReply* m_currentDownloadReply = nullptr;
    class QFile* m_currentDownloadFile = nullptr;
    QString m_currentSlug;
    QString m_currentVersion;
    QString m_currentLocalPath;
    int m_redirectsFollowed = 0;
};
