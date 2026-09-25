// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file clawhub-client.cpp
 * @brief Implementation of the ClawHub HTTP client: index fetch,
 *        skill download / install via SkillService, network error
 *        handling.
 * @layer Service
 * @dependencies SkillService, Qt6::Core, Qt6::Network.
 */

#include "clawhub-client.h"

#include "../utils/logger.h"

#include <QTimer>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QStandardPaths>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>

namespace {

constexpr int kSearchTimeoutMs = 30 * 1000;
constexpr int kDownloadTimeoutMs = 60 * 1000;
constexpr int kMaxRedirects = 3;

QByteArray makeUserAgent() {
    return QByteArrayLiteral("Verzeta-Studio (clawhub-client)");
}

}  // namespace

ClawHubClient::ClawHubClient(QObject* parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
    , m_apiBase(QStringLiteral("https://clawhub.ai/api/v1")) {}

ClawHubClient::~ClawHubClient() {
    cancelDownload();
}

void ClawHubClient::setApiBaseForTesting(const QString& base) {
    m_apiBase = base;
}

QString ClawHubClient::safeApiBase() const {
    return m_apiBase;
}

void ClawHubClient::setSearchInFlight(bool v) {
    if (m_searchInFlight == v)
        return;
    m_searchInFlight = v;
    emit searchInFlightChanged();
}

void ClawHubClient::setDownloadInFlight(bool v) {
    if (m_downloadInFlight == v)
        return;
    m_downloadInFlight = v;
    emit downloadInFlightChanged();
}

bool ClawHubClient::tlsAndHttpsCheck(QNetworkReply* reply, QString& err) {
    const QUrl url = reply->url();
    if (url.scheme() != QLatin1String("https") && url.scheme() != QLatin1String("http")) {
        err = QStringLiteral("non-http(s) scheme refused: %1").arg(url.scheme());
        return false;
    }
    // Allow http only for the test base (localhost / 127.x); production
    // uses https://clawhub.ai/api/v1.
    if (url.scheme() == QLatin1String("http")) {
        if (!(url.host() == QLatin1String("localhost") ||
              url.host().startsWith(QLatin1String("127.")))) {
            err = QStringLiteral("plain HTTP refused: %1").arg(url.toString());
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Search
// ---------------------------------------------------------------------------

void ClawHubClient::search(const QString& query, const QString& cursor, int limit) {
    setSearchInFlight(true);
    QUrl url(safeApiBase() + QStringLiteral("/search"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("q"), query);
    if (!cursor.isEmpty())
        q.addQueryItem(QStringLiteral("cursor"), cursor);
    q.addQueryItem(QStringLiteral("limit"), QString::number(limit));
    url.setQuery(q);

    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, makeUserAgent());
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setMaximumRedirectsAllowed(kMaxRedirects);

    QNetworkReply* reply = m_nam->get(req);
    QTimer::singleShot(kSearchTimeoutMs, reply, [reply]() {
        if (reply && reply->isRunning())
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, &ClawHubClient::onSearchFinished);
}

void ClawHubClient::onSearchFinished() {
    auto* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply)
        return;
    reply->deleteLater();
    setSearchInFlight(false);

    QString err;
    if (!tlsAndHttpsCheck(reply, err)) {
        emit searchFailed(err);
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        emit searchFailed(reply->errorString());
        return;
    }

    const QByteArray bytes = reply->readAll();
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        emit searchFailed(QStringLiteral("the server sent an unreadable response"));
        return;
    }
    const QJsonObject root = doc.object();
    // Variants per the reference Python script.
    QJsonArray arr = root.value(QStringLiteral("results")).toArray();
    if (arr.isEmpty())
        arr = root.value(QStringLiteral("skills")).toArray();
    if (arr.isEmpty())
        arr = root.value(QStringLiteral("items")).toArray();

    QVariantList results;
    for (const QJsonValue& v : arr) {
        results.append(parseSkillEnvelope(v.toObject()));
    }
    QString nextCursor = root.value(QStringLiteral("nextCursor")).toString();
    if (nextCursor.isEmpty())
        nextCursor = root.value(QStringLiteral("next_cursor")).toString();
    if (nextCursor.isEmpty())
        nextCursor = root.value(QStringLiteral("cursor")).toString();
    emit searchCompleted(results, nextCursor);

    // The /search endpoint returns minimal rows (slug + displayName +
    // summary) — owner, stats, moderation, latestVersion only show up
    // on /skills/<slug>. Fire one detail request per slug to enrich
    // the cards with the rest of the metadata. Failures are silent —
    // the un-enriched row stays in the search list with whatever the
    // /search endpoint provided.
    for (const QVariant& v : results) {
        const QString slug = v.toMap().value(QStringLiteral("slug")).toString();
        if (!slug.isEmpty())
            requestEnrichment(slug);
    }
}

QVariantMap ClawHubClient::parseSkillEnvelope(const QJsonObject& envelope) {
    // ClawHub wraps each detail response as
    //   {"skill": {...}, "owner": {...}, "moderation": {...},
    //    "latestVersion": {...}}.
    // The /search endpoint may return minimal flat rows (slug +
    // displayName + summary). Both shapes feed through here so the
    // QML side never sees a disjoint key set across paths.
    const QJsonObject skill = envelope.contains(QStringLiteral("skill"))
                                  ? envelope.value(QStringLiteral("skill")).toObject()
                                  : envelope;
    const QJsonObject owner = envelope.value(QStringLiteral("owner")).toObject();
    const QJsonObject moderation = envelope.value(QStringLiteral("moderation")).toObject();
    const QJsonObject latestVer = envelope.value(QStringLiteral("latestVersion")).toObject();
    const QJsonObject stats = skill.value(QStringLiteral("stats")).toObject();

    QVariantMap m;
    m.insert(QStringLiteral("slug"), skill.value("slug").toString());

    QString displayName = skill.value("displayName").toString();
    if (displayName.isEmpty())
        displayName = skill.value("name").toString();
    if (displayName.isEmpty())
        displayName = skill.value("slug").toString();
    m.insert(QStringLiteral("name"), displayName);

    QString summary = skill.value("summary").toString();
    if (summary.isEmpty())
        summary = skill.value("description").toString();
    m.insert(QStringLiteral("description"), summary);

    QString author = owner.value("handle").toString();
    if (author.isEmpty())
        author = owner.value("displayName").toString();
    if (author.isEmpty())
        author = skill.value("author").toString();
    m.insert(QStringLiteral("author"), author);

    const int downloads = stats.value("downloads").toInt(skill.value("downloads").toInt());
    const int stars = stats.value("stars").toInt();
    const int installsCurrent = stats.value("installsCurrent").toInt();
    const int versionsCount = stats.value("versions").toInt();
    m.insert(QStringLiteral("downloads"), downloads);
    m.insert(QStringLiteral("stars"), stars);
    m.insert(QStringLiteral("installsCurrent"), installsCurrent);
    m.insert(QStringLiteral("versionsCount"), versionsCount);
    m.insert(QStringLiteral("latestVersion"), latestVer.value("version").toString());

    QVariantMap versionTags;
    const QJsonObject tagsObj = skill.value("tags").toObject();
    for (auto it = tagsObj.begin(); it != tagsObj.end(); ++it) {
        versionTags.insert(it.key(), it.value().toString());
    }
    m.insert(QStringLiteral("versionTags"), versionTags);

    const bool isSuspicious = moderation.value("isSuspicious").toBool();
    const bool isMalwareBlocked = moderation.value("isMalwareBlocked").toBool();
    const QString verdict = moderation.value("verdict").toString();
    QStringList reasonCodes;
    for (const QJsonValue& rc : moderation.value("reasonCodes").toArray()) {
        reasonCodes.append(rc.toString());
    }
    m.insert(QStringLiteral("isSuspicious"), isSuspicious);
    m.insert(QStringLiteral("isMalwareBlocked"), isMalwareBlocked);
    m.insert(QStringLiteral("verdict"), verdict);
    m.insert(QStringLiteral("reasonCodes"), reasonCodes);
    return m;
}

void ClawHubClient::requestEnrichment(const QString& slug) {
    QUrl url(safeApiBase() + QStringLiteral("/skills/") + slug);
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, makeUserAgent());
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setMaximumRedirectsAllowed(kMaxRedirects);

    QNetworkReply* reply = m_nam->get(req);
    QTimer::singleShot(kSearchTimeoutMs, reply, [reply]() {
        if (reply && reply->isRunning())
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, [this, slug, reply]() {
        reply->deleteLater();
        QString tlsErr;
        if (!tlsAndHttpsCheck(reply, tlsErr))
            return;
        if (reply->error() != QNetworkReply::NoError)
            return;
        QJsonParseError pe;
        const QJsonDocument doc = QJsonDocument::fromJson(reply->readAll(), &pe);
        if (pe.error != QJsonParseError::NoError || !doc.isObject())
            return;
        const QVariantMap row = parseSkillEnvelope(doc.object());
        emit searchResultEnriched(slug, row);
    });
}

// ---------------------------------------------------------------------------
// Download
// ---------------------------------------------------------------------------

void ClawHubClient::downloadLatest(const QString& slug) {
    if (m_downloadInFlight) {
        emit downloadFailed(slug, QStringLiteral("another download is already running"));
        return;
    }
    setDownloadInFlight(true);
    m_currentSlug = slug;

    // Step 1: GET /skills/<slug> to resolve latest version.
    QUrl url(safeApiBase() + QStringLiteral("/skills/") + slug);
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, makeUserAgent());
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    req.setMaximumRedirectsAllowed(kMaxRedirects);
    QNetworkReply* reply = m_nam->get(req);
    QTimer::singleShot(kSearchTimeoutMs, reply, [reply]() {
        if (reply && reply->isRunning())
            reply->abort();
    });
    connect(reply, &QNetworkReply::finished, this, &ClawHubClient::onDetailFinished);
}

void ClawHubClient::onDetailFinished() {
    auto* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply)
        return;
    reply->deleteLater();

    QString err;
    if (!tlsAndHttpsCheck(reply, err)) {
        setDownloadInFlight(false);
        emit downloadFailed(m_currentSlug, err);
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        setDownloadInFlight(false);
        emit downloadFailed(m_currentSlug, reply->errorString());
        return;
    }
    const QByteArray bytes = reply->readAll();
    QJsonParseError pe;
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &pe);
    if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
        setDownloadInFlight(false);
        emit downloadFailed(m_currentSlug,
                            QStringLiteral("the server sent unreadable skill details"));
        return;
    }
    const QJsonObject root = doc.object();
    // Skill row may be wrapped or flat; tolerate both.
    QJsonObject skill = root.contains(QStringLiteral("skill"))
                            ? root.value(QStringLiteral("skill")).toObject()
                            : root;

    // Moderation gate. ClawHub's moderation engine flags malware-
    // blocked skills explicitly; we refuse to download those at the
    // client layer so the bad bytes never hit the user's machine.
    // `isSuspicious` is a softer signal — we propagate it as a
    // warning surfaced in installed-skills.json (S2 review-state
    // already retains warnings for the user's review dialog), but
    // do NOT block the download.
    const QJsonObject moderation = root.value(QStringLiteral("moderation")).toObject();
    if (moderation.value(QStringLiteral("isMalwareBlocked")).toBool()) {
        setDownloadInFlight(false);
        QString verdict = moderation.value(QStringLiteral("verdict")).toString();
        if (verdict.isEmpty())
            verdict = QStringLiteral("malware-blocked");
        emit downloadFailed(
            m_currentSlug,
            QStringLiteral("ClawHub flagged this skill as %1, so it was not downloaded.")
                .arg(verdict));
        return;
    }

    QString version = root.value(QStringLiteral("latestVersion"))
                          .toObject()
                          .value(QStringLiteral("version"))
                          .toString();
    if (version.isEmpty())
        version = skill.value(QStringLiteral("latestVersion"))
                      .toObject()
                      .value(QStringLiteral("version"))
                      .toString();
    if (version.isEmpty())
        version = skill.value(QStringLiteral("version")).toString();
    if (version.isEmpty()) {
        setDownloadInFlight(false);
        emit downloadFailed(m_currentSlug, QStringLiteral("could not find the latest version"));
        return;
    }
    m_currentVersion = version;

    // Step 2: GET /download?slug=&version=
    QUrl dlUrl(safeApiBase() + QStringLiteral("/download"));
    QUrlQuery q;
    q.addQueryItem(QStringLiteral("slug"), m_currentSlug);
    q.addQueryItem(QStringLiteral("version"), m_currentVersion);
    dlUrl.setQuery(q);

    const QString stageRoot = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                              QStringLiteral("/skills/.clawhub-staging");
    QDir().mkpath(stageRoot);
    m_currentLocalPath = stageRoot + QStringLiteral("/") + m_currentSlug + QStringLiteral("-") +
                         m_currentVersion + QStringLiteral("-") +
                         QUuid::createUuid().toString(QUuid::WithoutBraces) +
                         QStringLiteral(".zip");

    auto* file = new QFile(m_currentLocalPath, this);
    if (!file->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        delete file;
        setDownloadInFlight(false);
        emit downloadFailed(m_currentSlug, QStringLiteral("could not create the download file"));
        return;
    }
    m_currentDownloadFile = file;

    QNetworkRequest dlReq(dlUrl);
    dlReq.setHeader(QNetworkRequest::UserAgentHeader, makeUserAgent());
    dlReq.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                       QNetworkRequest::NoLessSafeRedirectPolicy);
    dlReq.setMaximumRedirectsAllowed(kMaxRedirects);

    QNetworkReply* dlReply = m_nam->get(dlReq);
    m_currentDownloadReply = dlReply;
    QTimer::singleShot(kDownloadTimeoutMs, dlReply, [dlReply]() {
        if (dlReply && dlReply->isRunning())
            dlReply->abort();
    });
    connect(dlReply, &QNetworkReply::readyRead, this, &ClawHubClient::onDownloadReadyRead);
    connect(dlReply, &QNetworkReply::finished, this, &ClawHubClient::onDownloadFinished);
}

void ClawHubClient::onDownloadReadyRead() {
    auto* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply || !m_currentDownloadFile)
        return;
    m_currentDownloadFile->write(reply->readAll());
}

void ClawHubClient::onDownloadFinished() {
    auto* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply)
        return;
    reply->deleteLater();
    setDownloadInFlight(false);

    if (m_currentDownloadFile) {
        m_currentDownloadFile->write(reply->readAll());
        m_currentDownloadFile->close();
        m_currentDownloadFile->deleteLater();
        m_currentDownloadFile = nullptr;
    }
    m_currentDownloadReply = nullptr;

    QString err;
    if (!tlsAndHttpsCheck(reply, err)) {
        QFile::remove(m_currentLocalPath);
        emit downloadFailed(m_currentSlug, err);
        return;
    }
    if (reply->error() != QNetworkReply::NoError) {
        QFile::remove(m_currentLocalPath);
        emit downloadFailed(m_currentSlug, reply->errorString());
        return;
    }
    emit downloadCompleted(m_currentSlug, m_currentVersion, m_currentLocalPath);
}

void ClawHubClient::cancelDownload() {
    if (m_currentDownloadReply) {
        m_currentDownloadReply->abort();
        m_currentDownloadReply->deleteLater();
        m_currentDownloadReply = nullptr;
    }
    if (m_currentDownloadFile) {
        m_currentDownloadFile->close();
        QFile::remove(m_currentLocalPath);
        m_currentDownloadFile->deleteLater();
        m_currentDownloadFile = nullptr;
    }
    setDownloadInFlight(false);
}
