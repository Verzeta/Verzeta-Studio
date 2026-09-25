// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file image-gen-worker.cpp
 * @brief Implementation of image generation via DALL-E API and local SD CLI.
 * @layer Worker
 * @dependencies QNetworkAccessManager, QProcess, QJsonDocument, Qt6::Core
 */


#include "image-gen-worker.h"

#include <QDir>
#include <QEventLoop>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QProcess>
#include <QStandardPaths>
#include <QUrl>
#include <QUrlQuery>
#include <QUuid>

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

/**
 * @brief Constructs ImageGenWorker.
 * @param parent Optional Qt parent.
 */
ImageGenWorker::ImageGenWorker(QObject* parent) : QObject(parent) {}

// ---------------------------------------------------------------------------
// Static helpers
// ---------------------------------------------------------------------------

QString ImageGenWorker::joinEndpoint(const QString& baseUrl, const QString& pathSuffix) {
    QString base = baseUrl.trimmed();
    while (base.endsWith(QLatin1Char('/')))
        base.chop(1);
    if (base.isEmpty()) {
        // Callers validate emptiness before dispatch; return the suffix so
        // a degenerate call still yields a syntactically-valid relative
        // path rather than crashing.
        return pathSuffix;
    }
    // If the user pasted the full documented endpoint (base already ends
    // with the suffix) do NOT append it again — that doubling is the
    // OpenRouter ".../chat/completions/chat/completions" → 404 bug.
    if (base.endsWith(pathSuffix, Qt::CaseInsensitive)) {
        return base;
    }
    return base + pathSuffix;
}

// ---------------------------------------------------------------------------
// Slots
// ---------------------------------------------------------------------------

/**
 * @brief Generates an image via OpenAI DALL-E 3 API.
 *
 * Flow:
 *  1. POST /v1/images/generations → parse data[0].url
 *  2. Emit progressUpdate(50)
 *  3. GET the image URL → save PNG bytes to disk
 *  4. Emit progressUpdate(100), then imageReady(localPath)
 *
 * @param prompt  Image description.
 * @param size    Dimensions ("1024x1024", "1024x1792", "1792x1024").
 * @param quality "standard" or "hd".
 * @param apiKey  OpenAI API key.
 */
void ImageGenWorker::generateViaOpenAI(const JobContext& ctx,
                                       const QString& prompt,
                                       const QString& size,
                                       const QString& quality,
                                       const QString& apiKey) {
    QNetworkAccessManager nam;

    // -----------------------------------------------------------------------
    // Step 1: POST to images/generations
    // -----------------------------------------------------------------------
    QJsonObject payload;
    payload[QStringLiteral("model")] = QStringLiteral("dall-e-3");
    payload[QStringLiteral("prompt")] = prompt;
    payload[QStringLiteral("size")] = size.isEmpty() ? QStringLiteral("1024x1024") : size;
    payload[QStringLiteral("quality")] = quality.isEmpty() ? QStringLiteral("standard") : quality;
    payload[QStringLiteral("n")] = 1;

    QNetworkRequest genReq(QUrl(QStringLiteral("https://api.openai.com/v1/images/generations")));
    genReq.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    genReq.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(apiKey).toUtf8());

    QEventLoop loop1;
    QNetworkReply* genReply =
        nam.post(genReq, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    QObject::connect(genReply, &QNetworkReply::finished, &loop1, &QEventLoop::quit);
    loop1.exec();

    if (genReply->error() != QNetworkReply::NoError) {
        const QString msg = genReply->errorString();
        genReply->deleteLater();
        emit errorOccurred(ctx, msg);
        return;
    }

    const QByteArray genData = genReply->readAll();
    genReply->deleteLater();

    const QJsonDocument genDoc = QJsonDocument::fromJson(genData);
    if (genDoc.isNull() || !genDoc.isObject()) {
        emit errorOccurred(ctx, QStringLiteral("DALL-E: invalid JSON response"));
        return;
    }

    const QJsonArray dataArr = genDoc.object().value(QStringLiteral("data")).toArray();
    if (dataArr.isEmpty()) {
        // Check for API error object
        const QJsonObject errObj = genDoc.object().value(QStringLiteral("error")).toObject();
        const QString errMsg = errObj.value(QStringLiteral("message"))
                                   .toString(QStringLiteral("DALL-E: empty data array"));
        emit errorOccurred(ctx, errMsg);
        return;
    }

    const QString imageUrl = dataArr.first().toObject().value(QStringLiteral("url")).toString();
    if (imageUrl.isEmpty()) {
        emit errorOccurred(ctx, QStringLiteral("DALL-E: missing image URL in response"));
        return;
    }

    emit progressUpdate(ctx, 50);

    // -----------------------------------------------------------------------
    // Step 2: Download the image
    // -----------------------------------------------------------------------
    QNetworkRequest dlReq{QUrl(imageUrl)};
    QEventLoop loop2;
    QNetworkReply* dlReply = nam.get(dlReq);
    QObject::connect(dlReply, &QNetworkReply::finished, &loop2, &QEventLoop::quit);
    loop2.exec();

    if (dlReply->error() != QNetworkReply::NoError) {
        const QString msg = dlReply->errorString();
        dlReply->deleteLater();
        emit errorOccurred(ctx, msg);
        return;
    }

    const QByteArray imageBytes = dlReply->readAll();
    dlReply->deleteLater();

    // -----------------------------------------------------------------------
    // Step 3: Save to disk
    // -----------------------------------------------------------------------
    const QString attachDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                              QStringLiteral("/attachments");
    QDir().mkpath(attachDir);

    const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString savePath = attachDir + QStringLiteral("/") + uuid + QStringLiteral(".png");

    QFile f(savePath);
    if (!f.open(QIODevice::WriteOnly)) {
        emit errorOccurred(ctx, QStringLiteral("Cannot write image file: ") + savePath);
        return;
    }
    f.write(imageBytes);
    f.close();

    emit progressUpdate(ctx, 100);
    emit imageReady(ctx, savePath);
}

/**
 * @brief Generates an image via a local Stable Diffusion CLI.
 *
 * Spawns: {sdPath} --prompt "{prompt}" --output {outputPath} [--steps N] [--seed S]
 * Parses stdout lines of the form "Step N/M" to emit progress.
 *
 * @param prompt  Image description.
 * @param sdPath  Absolute path to the SD CLI executable.
 * @param params  Additional parameters: {"steps": int, "seed": int}.
 */
void ImageGenWorker::generateViaLocalSD(const JobContext& ctx,
                                        const QString& prompt,
                                        const QString& sdPath,
                                        const QJsonObject& params) {
    const QString attachDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                              QStringLiteral("/attachments");
    QDir().mkpath(attachDir);

    const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString outputPath = attachDir + QStringLiteral("/") + uuid + QStringLiteral(".png");

    const int steps = params.value(QStringLiteral("steps")).toInt(30);

    QStringList args;
    args << QStringLiteral("--prompt") << prompt << QStringLiteral("--output") << outputPath
         << QStringLiteral("--steps") << QString::number(steps);

    if (params.contains(QStringLiteral("seed"))) {
        args << QStringLiteral("--seed")
             << QString::number(params.value(QStringLiteral("seed")).toInt(-1));
    }

    QProcess proc;
    connect(&proc, &QProcess::readyReadStandardOutput, this, [&proc, steps, ctx, this]() {
        const QString out = QString::fromUtf8(proc.readAllStandardOutput());
        // Parse "Step N/M" progress lines
        const QStringList lines = out.split(QLatin1Char('\n'));
        for (const QString& line : lines) {
            // Match "Step 15/50" or "Step 15 / 50"
            const int slashIdx = line.indexOf(QLatin1Char('/'));
            const int stepIdx = line.toLower().indexOf(QStringLiteral("step "));
            if (stepIdx != -1 && slashIdx != -1 && slashIdx > stepIdx) {
                bool ok = false;
                const int current =
                    line.mid(stepIdx + 5, slashIdx - stepIdx - 5).trimmed().toInt(&ok);
                if (ok && steps > 0) {
                    emit progressUpdate(ctx, qMin(99, (current * 100) / steps));
                }
            }
        }
    });

    proc.start(sdPath, args);
    if (!proc.waitForStarted(5000)) {
        emit errorOccurred(ctx, QStringLiteral("Local SD: failed to start process: ") + sdPath);
        return;
    }

    proc.waitForFinished(-1);

    if (proc.exitCode() != 0) {
        const QString errOut = QString::fromUtf8(proc.readAllStandardError());
        emit errorOccurred(ctx,
                           QStringLiteral("Local SD exited with code %1: %2")
                               .arg(proc.exitCode())
                               .arg(errOut.trimmed()));
        return;
    }

    if (!QFile::exists(outputPath)) {
        emit errorOccurred(ctx, QStringLiteral("Local SD: output file not found: ") + outputPath);
        return;
    }

    emit progressUpdate(ctx, 100);
    emit imageReady(ctx, outputPath);
}


namespace {

/** Save raw bytes to AppDataLocation/attachments/\<uuid\>.png and return
 *  the absolute path. Empty return on failure (caller emits error). */
QString saveImageBytes(const QByteArray& bytes) {
    const QString attachDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) +
                              QStringLiteral("/attachments");
    QDir().mkpath(attachDir);

    const QString uuid = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString savePath = attachDir + QStringLiteral("/") + uuid + QStringLiteral(".png");

    QFile f(savePath);
    if (!f.open(QIODevice::WriteOnly))
        return QString();
    f.write(bytes);
    f.close();
    return savePath;
}

/** Read an image file and return it as a "data:<mime>;base64,…" URL for
 *  embedding as a chat-image reference (refine/edit). Empty on read
 *  failure. Caps the read at 16 MiB. Mime is inferred from the extension
 *  (defaults to image/png because our generated images are PNG). */
QString fileToDataUrl(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return QString();
    const QByteArray bytes = f.read(16 * 1024 * 1024);
    f.close();
    if (bytes.isEmpty())
        return QString();
    QString mime = QStringLiteral("image/png");
    const QString lower = path.toLower();
    if (lower.endsWith(QStringLiteral(".jpg")) || lower.endsWith(QStringLiteral(".jpeg"))) {
        mime = QStringLiteral("image/jpeg");
    } else if (lower.endsWith(QStringLiteral(".webp"))) {
        mime = QStringLiteral("image/webp");
    } else if (lower.endsWith(QStringLiteral(".gif"))) {
        mime = QStringLiteral("image/gif");
    }
    return QStringLiteral("data:%1;base64,%2").arg(mime, QString::fromLatin1(bytes.toBase64()));
}

/** Parse "WxH" → {W,H}. Falls back to {fallbackW, fallbackH} on any
 *  parse error so the worker never crashes on a malformed size string. */
QPair<int, int> parseSize(const QString& size, int fallbackW, int fallbackH) {
    const int xIdx = size.indexOf(QLatin1Char('x'));
    if (xIdx <= 0)
        return {fallbackW, fallbackH};
    bool okW = false, okH = false;
    const int w = size.left(xIdx).toInt(&okW);
    const int h = size.mid(xIdx + 1).toInt(&okH);
    if (!okW || !okH || w <= 0 || h <= 0) {
        return {fallbackW, fallbackH};
    }
    return {w, h};
}

/**
 * @brief Build the request URL and apply header/query credential per
 *        @p authLocation.
 *
 * Appends @p pathSuffix to the (slash-trimmed) @p baseUrl (idempotently,
 * via joinEndpoint). When @p key is non-empty:
 *   - "query"  → the credential is attached as the @p authQueryParam URL
 *                query parameter; no header is set.
 *   - "header" → the header @p authHeaderName is set to
 *                @p authValuePrefix + key (default location).
 *   - "body"   → NOTHING is applied here; the caller injects the
 *                credential into the JSON payload (see injectBodyAuth).
 *
 * @returns The resolved QUrl. Any header is applied to @p req.
 */
QUrl buildEndpointWithAuth(QNetworkRequest& req,
                           const QString& baseUrl,
                           const QString& pathSuffix,
                           const QString& key,
                           const QString& authLocation,
                           const QString& authHeaderName,
                           const QString& authValuePrefix,
                           const QString& authQueryParam) {
    QUrl url(ImageGenWorker::joinEndpoint(baseUrl, pathSuffix));
    if (key.isEmpty()) {
        return url;
    }
    if (authLocation == QStringLiteral("query")) {
        if (!authQueryParam.isEmpty()) {
            QUrlQuery q(url);
            q.addQueryItem(authQueryParam, key);
            url.setQuery(q);
        }
    } else if (authLocation == QStringLiteral("body")) {
        // Injected into the JSON payload by the caller, not here.
    } else {
        const QString header =
            authHeaderName.isEmpty() ? QStringLiteral("Authorization") : authHeaderName;
        req.setRawHeader(header.toUtf8(), (authValuePrefix + key).toUtf8());
    }
    return url;
}

/**
 * @brief Merges the credential into @p payload as {\<authBodyField\>: key}
 *        when @p authLocation == "body" and @p key is non-empty.
 *
 * No-op for header/query auth. The field name falls back to "api_key"
 * when @p authBodyField is empty.
 */
void injectBodyAuth(QJsonObject& payload,
                    const QString& authLocation,
                    const QString& authBodyField,
                    const QString& key) {
    if (authLocation != QStringLiteral("body") || key.isEmpty()) {
        return;
    }
    const QString field = authBodyField.isEmpty() ? QStringLiteral("api_key") : authBodyField;
    payload[field] = key;
}

/**
 * @brief Decode an image from a data: URL or a raw base64 string.
 *
 * Accepts "data:image/png;base64,...." (strips the prefix) or a bare
 * base64 payload. Returns empty on decode failure.
 */
QByteArray decodeImagePayload(const QString& payload) {
    QString b64 = payload.trimmed();
    if (b64.startsWith(QStringLiteral("data:"))) {
        const int commaIdx = b64.indexOf(QLatin1Char(','));
        if (commaIdx > 0) {
            b64 = b64.mid(commaIdx + 1);
        }
    }
    return QByteArray::fromBase64(b64.toUtf8());
}

/**
 * @brief Extract a generated image (as a data URL or http URL) from a
 *        chat-completions response message.
 *
 * Looks first at message.images[].image_url.url (OpenRouter / chat-image
 * shape), then falls back to a markdown image / data URL embedded in
 * message.content. Returns empty when nothing image-like is found.
 */
QString extractChatImageUrl(const QJsonObject& message) {
    // 1. message.images[] — OpenRouter / OpenAI chat-image shape.
    const QJsonArray images = message.value(QStringLiteral("images")).toArray();
    for (const QJsonValue& v : images) {
        const QJsonObject imgObj = v.toObject();
        const QJsonValue iu = imgObj.value(QStringLiteral("image_url"));
        if (iu.isObject()) {
            const QString u = iu.toObject().value(QStringLiteral("url")).toString();
            if (!u.isEmpty())
                return u;
        } else if (iu.isString() && !iu.toString().isEmpty()) {
            return iu.toString();
        }
        // Some providers put the data URL straight on "url".
        const QString direct = imgObj.value(QStringLiteral("url")).toString();
        if (!direct.isEmpty())
            return direct;
    }

    // 2. message.content — may be a string holding a markdown image
    //    (![alt](data:...)) or a bare data: URL, or an array of parts.
    auto scanString = [](const QString& s) -> QString {
        // Markdown image: ![..](URL)
        const int mdOpen = s.indexOf(QStringLiteral("]("));
        if (mdOpen >= 0) {
            const int urlStart = mdOpen + 2;
            const int urlEnd = s.indexOf(QLatin1Char(')'), urlStart);
            if (urlEnd > urlStart) {
                const QString u = s.mid(urlStart, urlEnd - urlStart).trimmed();
                if (u.startsWith(QStringLiteral("data:")) || u.startsWith(QStringLiteral("http"))) {
                    return u;
                }
            }
        }
        const QString t = s.trimmed();
        if (t.startsWith(QStringLiteral("data:image")))
            return t;
        return QString();
    };

    const QJsonValue content = message.value(QStringLiteral("content"));
    if (content.isString()) {
        const QString hit = scanString(content.toString());
        if (!hit.isEmpty())
            return hit;
    } else if (content.isArray()) {
        for (const QJsonValue& part : content.toArray()) {
            const QJsonObject po = part.toObject();
            // OpenAI-style image part.
            const QJsonValue iu = po.value(QStringLiteral("image_url"));
            if (iu.isObject()) {
                const QString u = iu.toObject().value(QStringLiteral("url")).toString();
                if (!u.isEmpty())
                    return u;
            }
            const QString txt = po.value(QStringLiteral("text")).toString();
            if (!txt.isEmpty()) {
                const QString hit = scanString(txt);
                if (!hit.isEmpty())
                    return hit;
            }
        }
    }
    return QString();
}

}  // namespace

/**
 * @brief Generates an image via a self-hosted OpenAI-compatible HTTP
 *        server (SD.Next, Forge in OpenAI mode, ComfyUI proxy).
 *
 * @param prompt  Image description.
 * @param url     Base URL (no trailing path).
 * @param apiKey  Optional bearer token.
 * @param size    "WxH".
 * @param quality "standard" / "hd".
 */
void ImageGenWorker::generateViaOpenAICompat(const JobContext& ctx,
                                             const QString& prompt,
                                             const QString& url,
                                             const QString& apiKey,
                                             const QString& size,
                                             const QString& quality) {
    if (url.isEmpty()) {
        emit errorOccurred(ctx, QStringLiteral("OpenAI-compatible image server URL is empty."));
        return;
    }
    QNetworkAccessManager nam;

    // Accept either the API base or the full ".../images/generations" URL.
    const QUrl endpoint(joinEndpoint(url, QStringLiteral("/images/generations")));
    if (!endpoint.isValid()) {
        emit errorOccurred(ctx, QStringLiteral("Invalid server URL: ") + url);
        return;
    }

    QJsonObject payload;
    payload[QStringLiteral("prompt")] = prompt;
    payload[QStringLiteral("size")] = size.isEmpty() ? QStringLiteral("1024x1024") : size;
    payload[QStringLiteral("quality")] = quality.isEmpty() ? QStringLiteral("standard") : quality;
    payload[QStringLiteral("n")] = 1;
    // Ask for inline base64 so we don't need a follow-up download GET
    // (many self-hosted servers return a local file:// URL that the
    // host can't reach anyway).
    payload[QStringLiteral("response_format")] = QStringLiteral("b64_json");

    QNetworkRequest req(endpoint);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    if (!apiKey.isEmpty()) {
        req.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(apiKey).toUtf8());
    }

    QEventLoop loop;
    QNetworkReply* reply = nam.post(req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    if (reply->error() != QNetworkReply::NoError) {
        const QString msg = reply->errorString();
        reply->deleteLater();
        emit errorOccurred(ctx, QStringLiteral("OpenAI-compatible server: ") + msg);
        return;
    }

    const QByteArray body = reply->readAll();
    reply->deleteLater();

    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (doc.isNull() || !doc.isObject()) {
        emit errorOccurred(ctx, QStringLiteral("OpenAI-compatible server: invalid JSON response"));
        return;
    }

    const QJsonObject root = doc.object();
    const QJsonArray dataArr = root.value(QStringLiteral("data")).toArray();
    if (dataArr.isEmpty()) {
        const QJsonObject errObj = root.value(QStringLiteral("error")).toObject();
        const QString errMsg =
            errObj.value(QStringLiteral("message"))
                .toString(QStringLiteral("OpenAI-compatible server: empty data array"));
        emit errorOccurred(ctx, errMsg);
        return;
    }
    const QJsonObject first = dataArr.first().toObject();

    emit progressUpdate(ctx, 70);

    // Prefer b64_json (no second round-trip); fall back to url.
    const QString b64 = first.value(QStringLiteral("b64_json")).toString();
    QByteArray imageBytes;
    if (!b64.isEmpty()) {
        imageBytes = QByteArray::fromBase64(b64.toUtf8());
    } else {
        const QString imageUrl = first.value(QStringLiteral("url")).toString();
        if (imageUrl.isEmpty()) {
            emit errorOccurred(
                ctx,
                QStringLiteral("OpenAI-compatible server: response missing both b64_json and url"));
            return;
        }
        QEventLoop loop2;
        QNetworkReply* dl = nam.get(QNetworkRequest(QUrl(imageUrl)));
        QObject::connect(dl, &QNetworkReply::finished, &loop2, &QEventLoop::quit);
        loop2.exec();
        if (dl->error() != QNetworkReply::NoError) {
            const QString msg = dl->errorString();
            dl->deleteLater();
            emit errorOccurred(ctx, QStringLiteral("OpenAI-compatible server download: ") + msg);
            return;
        }
        imageBytes = dl->readAll();
        dl->deleteLater();
    }

    if (imageBytes.isEmpty()) {
        emit errorOccurred(ctx, QStringLiteral("OpenAI-compatible server: decoded image is empty"));
        return;
    }

    const QString savePath = saveImageBytes(imageBytes);
    if (savePath.isEmpty()) {
        emit errorOccurred(ctx,
                           QStringLiteral("OpenAI-compatible server: cannot write image to disk"));
        return;
    }
    emit progressUpdate(ctx, 100);
    emit imageReady(ctx, savePath);
}

/**
 * @brief Generates an image via an Automatic1111 / SD WebUI server.
 *
 * @param prompt  Image description.
 * @param url     Base URL (no trailing path).
 * @param apiKey  Optional bearer token.
 * @param size    "WxH", parsed into A1111's width/height fields.
 * @param params  Optional steps, cfg_scale, sampler_name, seed,
 *                negative_prompt. Unknown keys pass through to A1111.
 */
void ImageGenWorker::generateViaAutomatic1111(const JobContext& ctx,
                                              const QString& prompt,
                                              const QString& url,
                                              const QString& apiKey,
                                              const QString& size,
                                              const QJsonObject& params) {
    if (url.isEmpty()) {
        emit errorOccurred(ctx, QStringLiteral("Automatic1111 server URL is empty."));
        return;
    }
    QNetworkAccessManager nam;

    // Accept either the API base or the full ".../sdapi/v1/txt2img" URL.
    const QUrl endpoint(joinEndpoint(url, QStringLiteral("/sdapi/v1/txt2img")));
    if (!endpoint.isValid()) {
        emit errorOccurred(ctx, QStringLiteral("Invalid server URL: ") + url);
        return;
    }

    const auto wh = parseSize(size, 512, 512);

    // Start from params so unknown keys pass through (e.g. hires_fix,
    // batch_size). Overwrite the well-known ones with our resolved
    // values so callers can't accidentally override prompt / w / h.
    QJsonObject payload = params;
    payload[QStringLiteral("prompt")] = prompt;
    payload[QStringLiteral("width")] = wh.first;
    payload[QStringLiteral("height")] = wh.second;
    payload[QStringLiteral("n_iter")] = 1;
    if (!payload.contains(QStringLiteral("steps"))) {
        payload[QStringLiteral("steps")] = 30;
    }

    QNetworkRequest req(endpoint);
    req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));
    if (!apiKey.isEmpty()) {
        req.setRawHeader("Authorization", QStringLiteral("Bearer %1").arg(apiKey).toUtf8());
    }

    QEventLoop loop;
    QNetworkReply* reply = nam.post(req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();

    if (reply->error() != QNetworkReply::NoError) {
        const QString msg = reply->errorString();
        reply->deleteLater();
        emit errorOccurred(ctx, QStringLiteral("A1111: ") + msg);
        return;
    }

    const QByteArray body = reply->readAll();
    reply->deleteLater();

    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (doc.isNull() || !doc.isObject()) {
        emit errorOccurred(ctx, QStringLiteral("A1111: invalid JSON response"));
        return;
    }

    const QJsonObject root = doc.object();
    const QJsonArray imgArr = root.value(QStringLiteral("images")).toArray();
    if (imgArr.isEmpty()) {
        // A1111 surfaces server errors in the "detail" or "error" fields.
        const QString errMsg =
            root.value(QStringLiteral("error"))
                .toString(root.value(QStringLiteral("detail"))
                              .toString(QStringLiteral("A1111: empty images array in response")));
        emit errorOccurred(ctx, errMsg);
        return;
    }

    emit progressUpdate(ctx, 70);

    QString b64 = imgArr.first().toString();
    // A1111 sometimes returns a data URI prefix; strip if present.
    const int commaIdx = b64.indexOf(QLatin1Char(','));
    if (b64.startsWith(QStringLiteral("data:")) && commaIdx > 0) {
        b64 = b64.mid(commaIdx + 1);
    }

    const QByteArray imageBytes = QByteArray::fromBase64(b64.toUtf8());
    if (imageBytes.isEmpty()) {
        emit errorOccurred(ctx, QStringLiteral("A1111: failed to decode base64 image"));
        return;
    }

    const QString savePath = saveImageBytes(imageBytes);
    if (savePath.isEmpty()) {
        emit errorOccurred(ctx, QStringLiteral("A1111: cannot write image to disk"));
        return;
    }
    emit progressUpdate(ctx, 100);
    emit imageReady(ctx, savePath);
}

// ---------------------------------------------------------------------------
// Unified registry-driven dispatch (shape-based)
// ---------------------------------------------------------------------------

/**
 * @brief Routes a resolved config to the matching shape handler.
 *
 * See the header for the per-shape contract. Every arm threads `ctx`
 * through unchanged and applies flexible auth via buildEndpointWithAuth.
 */
void ImageGenWorker::generate(const JobContext& ctx,
                              const QString& prompt,
                              const QJsonObject& params) {
    const QString shape = params.value(QStringLiteral("endpointShape")).toString();
    const QString baseUrl = params.value(QStringLiteral("baseUrl")).toString();
    const QString model = params.value(QStringLiteral("model")).toString();
    const QString size = params.value(QStringLiteral("size")).toString();
    const QString quality = params.value(QStringLiteral("quality")).toString();
    const QString apiKey = params.value(QStringLiteral("apiKey")).toString();
    const QString authLocation = params.value(QStringLiteral("authLocation")).toString();
    const QString authHeaderName = params.value(QStringLiteral("authHeaderName")).toString();
    const QString authValuePrefix = params.value(QStringLiteral("authValuePrefix")).toString();
    const QString authQueryParam = params.value(QStringLiteral("authQueryParam")).toString();
    const QString authBodyField = params.value(QStringLiteral("authBodyField")).toString();
    const QString sdPath = params.value(QStringLiteral("sdPath")).toString();
    const QString sourceImagePath = params.value(QStringLiteral("sourceImagePath")).toString();
    const QJsonObject extra = params.value(QStringLiteral("extraParams")).toObject();

    // local_cli reuses the existing CLI spawn path verbatim.
    if (shape == QStringLiteral("local_cli")) {
        generateViaLocalSD(ctx, prompt, sdPath, extra);
        return;
    }

    if (baseUrl.isEmpty()) {
        emit errorOccurred(ctx, QStringLiteral("Image provider base URL is empty."));
        return;
    }

    QNetworkAccessManager nam;

    // -----------------------------------------------------------------------
    // a1111 — POST {baseUrl}/sdapi/v1/txt2img, parse images[0] base64.
    // -----------------------------------------------------------------------
    if (shape == QStringLiteral("a1111")) {
        QNetworkRequest req;
        const QUrl endpoint = buildEndpointWithAuth(req,
                                                    baseUrl,
                                                    QStringLiteral("/sdapi/v1/txt2img"),
                                                    apiKey,
                                                    authLocation,
                                                    authHeaderName,
                                                    authValuePrefix,
                                                    authQueryParam);
        if (!endpoint.isValid()) {
            emit errorOccurred(ctx, QStringLiteral("Invalid server URL: ") + baseUrl);
            return;
        }
        req.setUrl(endpoint);
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

        const auto wh = parseSize(size, 512, 512);
        QJsonObject payload = extra;
        payload[QStringLiteral("prompt")] = prompt;
        payload[QStringLiteral("width")] = wh.first;
        payload[QStringLiteral("height")] = wh.second;
        payload[QStringLiteral("n_iter")] = 1;
        if (!payload.contains(QStringLiteral("steps"))) {
            payload[QStringLiteral("steps")] = 30;
        }
        injectBodyAuth(payload, authLocation, authBodyField, apiKey);

        QEventLoop loop;
        QNetworkReply* reply = nam.post(req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        if (reply->error() != QNetworkReply::NoError) {
            const QString msg = reply->errorString();
            reply->deleteLater();
            emit errorOccurred(ctx, QStringLiteral("A1111: ") + msg);
            return;
        }
        const QByteArray body = reply->readAll();
        reply->deleteLater();

        const QJsonDocument doc = QJsonDocument::fromJson(body);
        if (doc.isNull() || !doc.isObject()) {
            emit errorOccurred(ctx, QStringLiteral("A1111: invalid JSON response"));
            return;
        }
        const QJsonObject root = doc.object();
        const QJsonArray imgArr = root.value(QStringLiteral("images")).toArray();
        if (imgArr.isEmpty()) {
            const QString errMsg =
                root.value(QStringLiteral("error"))
                    .toString(
                        root.value(QStringLiteral("detail"))
                            .toString(QStringLiteral("A1111: empty images array in response")));
            emit errorOccurred(ctx, errMsg);
            return;
        }
        emit progressUpdate(ctx, 70);
        const QByteArray imageBytes = decodeImagePayload(imgArr.first().toString());
        if (imageBytes.isEmpty()) {
            emit errorOccurred(ctx, QStringLiteral("A1111: failed to decode base64 image"));
            return;
        }
        const QString savePath = saveImageBytes(imageBytes);
        if (savePath.isEmpty()) {
            emit errorOccurred(ctx, QStringLiteral("A1111: cannot write image to disk"));
            return;
        }
        emit progressUpdate(ctx, 100);
        emit imageReady(ctx, savePath);
        return;
    }

    // -----------------------------------------------------------------------
    // openai_chat_image — POST {baseUrl}/chat/completions with
    // modalities ["image","text"] (OpenRouter and other chat-image
    // providers). Parse the image out of the response message.
    // -----------------------------------------------------------------------
    if (shape == QStringLiteral("openai_chat_image")) {
        QNetworkRequest req;
        const QUrl endpoint = buildEndpointWithAuth(req,
                                                    baseUrl,
                                                    QStringLiteral("/chat/completions"),
                                                    apiKey,
                                                    authLocation,
                                                    authHeaderName,
                                                    authValuePrefix,
                                                    authQueryParam);
        if (!endpoint.isValid()) {
            emit errorOccurred(ctx, QStringLiteral("Invalid server URL: ") + baseUrl);
            return;
        }
        req.setUrl(endpoint);
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

        QJsonObject userMsg;
        userMsg[QStringLiteral("role")] = QStringLiteral("user");
        if (!sourceImagePath.isEmpty()) {
            // Refine/edit: send the existing image as a reference next to
            // the instruction so the model edits it (multimodal content).
            const QString dataUrl = fileToDataUrl(sourceImagePath);
            if (dataUrl.isEmpty()) {
                emit errorOccurred(ctx, QStringLiteral("Refine: could not read the source image."));
                return;
            }
            QJsonObject textPart;
            textPart[QStringLiteral("type")] = QStringLiteral("text");
            textPart[QStringLiteral("text")] = prompt;
            QJsonObject imgPart;
            imgPart[QStringLiteral("type")] = QStringLiteral("image_url");
            QJsonObject iu;
            iu[QStringLiteral("url")] = dataUrl;
            imgPart[QStringLiteral("image_url")] = iu;
            QJsonArray content;
            content.append(textPart);
            content.append(imgPart);
            userMsg[QStringLiteral("content")] = content;
        } else {
            userMsg[QStringLiteral("content")] = prompt;
        }
        QJsonArray messages;
        messages.append(userMsg);

        QJsonObject payload;
        if (!model.isEmpty()) {
            payload[QStringLiteral("model")] = model;
        }
        payload[QStringLiteral("messages")] = messages;
        // Image-only models (Flux, grok-imagine, …) reject a request that
        // also asks for TEXT output ("No endpoints found that support the
        // requested output modalities: image, text"). Only dual-output
        // models (e.g. Gemini) accept ["image","text"]. The provider's
        // outputModalities setting picks; default is image-only.
        const QString outMods = params.value(QStringLiteral("outputModalities")).toString();
        QJsonArray modalities;
        modalities.append(QStringLiteral("image"));
        if (outMods == QStringLiteral("image_text")) {
            modalities.append(QStringLiteral("text"));
        }
        payload[QStringLiteral("modalities")] = modalities;
        injectBodyAuth(payload, authLocation, authBodyField, apiKey);

        QEventLoop loop;
        QNetworkReply* reply = nam.post(req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        if (reply->error() != QNetworkReply::NoError) {
            // Surface the server's JSON error body when present — it is
            // far more useful than the generic transport string.
            const QByteArray errBody = reply->readAll();
            const QString transport = reply->errorString();
            reply->deleteLater();
            QString detail = transport;
            const QJsonObject eo = QJsonDocument::fromJson(errBody).object();
            const QString apiErr = eo.value(QStringLiteral("error"))
                                       .toObject()
                                       .value(QStringLiteral("message"))
                                       .toString();
            if (!apiErr.isEmpty())
                detail = apiErr;
            emit errorOccurred(ctx, QStringLiteral("Chat-image: ") + detail);
            return;
        }
        const QByteArray body = reply->readAll();
        reply->deleteLater();

        const QJsonDocument doc = QJsonDocument::fromJson(body);
        if (doc.isNull() || !doc.isObject()) {
            emit errorOccurred(ctx, QStringLiteral("Chat-image: invalid JSON response"));
            return;
        }
        const QJsonObject root = doc.object();
        const QJsonArray choices = root.value(QStringLiteral("choices")).toArray();
        if (choices.isEmpty()) {
            const QString apiErr = root.value(QStringLiteral("error"))
                                       .toObject()
                                       .value(QStringLiteral("message"))
                                       .toString(QStringLiteral("Chat-image: no choices in "
                                                                "response"));
            emit errorOccurred(ctx, apiErr);
            return;
        }
        const QJsonObject message =
            choices.first().toObject().value(QStringLiteral("message")).toObject();
        const QString imageRef = extractChatImageUrl(message);
        if (imageRef.isEmpty()) {
            emit errorOccurred(
                ctx,
                QStringLiteral("Chat-image: response contained no image. The model may "
                               "not support image output, or the request needs a "
                               "different model."));
            return;
        }

        emit progressUpdate(ctx, 70);

        QByteArray imageBytes;
        if (imageRef.startsWith(QStringLiteral("data:"))) {
            imageBytes = decodeImagePayload(imageRef);
        } else {
            // Remote URL — download it.
            QEventLoop dlLoop;
            QNetworkReply* dl = nam.get(QNetworkRequest(QUrl(imageRef)));
            QObject::connect(dl, &QNetworkReply::finished, &dlLoop, &QEventLoop::quit);
            dlLoop.exec();
            if (dl->error() != QNetworkReply::NoError) {
                const QString msg = dl->errorString();
                dl->deleteLater();
                emit errorOccurred(ctx, QStringLiteral("Chat-image download: ") + msg);
                return;
            }
            imageBytes = dl->readAll();
            dl->deleteLater();
        }
        if (imageBytes.isEmpty()) {
            emit errorOccurred(ctx, QStringLiteral("Chat-image: decoded image is empty"));
            return;
        }
        const QString savePath = saveImageBytes(imageBytes);
        if (savePath.isEmpty()) {
            emit errorOccurred(ctx, QStringLiteral("Chat-image: cannot write image to disk"));
            return;
        }
        emit progressUpdate(ctx, 100);
        emit imageReady(ctx, savePath);
        return;
    }

    // -----------------------------------------------------------------------
    // openai_images (default) — POST {baseUrl}/images/generations.
    // Parse data[0].b64_json (preferred) or data[0].url (downloaded).
    // -----------------------------------------------------------------------
    if (shape != QStringLiteral("openai_images")) {
        emit errorOccurred(ctx, QStringLiteral("Unsupported image endpoint type: ") + shape);
        return;
    }
    {
        QNetworkRequest req;
        const QUrl endpoint = buildEndpointWithAuth(req,
                                                    baseUrl,
                                                    QStringLiteral("/images/generations"),
                                                    apiKey,
                                                    authLocation,
                                                    authHeaderName,
                                                    authValuePrefix,
                                                    authQueryParam);
        if (!endpoint.isValid()) {
            emit errorOccurred(ctx, QStringLiteral("Invalid server URL: ") + baseUrl);
            return;
        }
        req.setUrl(endpoint);
        req.setHeader(QNetworkRequest::ContentTypeHeader, QStringLiteral("application/json"));

        QJsonObject payload;
        if (!model.isEmpty()) {
            payload[QStringLiteral("model")] = model;
        }
        payload[QStringLiteral("prompt")] = prompt;
        payload[QStringLiteral("size")] = size.isEmpty() ? QStringLiteral("1024x1024") : size;
        payload[QStringLiteral("quality")] =
            quality.isEmpty() ? QStringLiteral("standard") : quality;
        payload[QStringLiteral("n")] = 1;
        payload[QStringLiteral("response_format")] = QStringLiteral("b64_json");
        injectBodyAuth(payload, authLocation, authBodyField, apiKey);

        QEventLoop loop;
        QNetworkReply* reply = nam.post(req, QJsonDocument(payload).toJson(QJsonDocument::Compact));
        QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
        loop.exec();
        if (reply->error() != QNetworkReply::NoError) {
            const QByteArray errBody = reply->readAll();
            const QString transport = reply->errorString();
            reply->deleteLater();
            QString detail = transport;
            const QString apiErr = QJsonDocument::fromJson(errBody)
                                       .object()
                                       .value(QStringLiteral("error"))
                                       .toObject()
                                       .value(QStringLiteral("message"))
                                       .toString();
            if (!apiErr.isEmpty())
                detail = apiErr;
            emit errorOccurred(ctx, QStringLiteral("OpenAI Images: ") + detail);
            return;
        }
        const QByteArray body = reply->readAll();
        reply->deleteLater();

        const QJsonDocument doc = QJsonDocument::fromJson(body);
        if (doc.isNull() || !doc.isObject()) {
            emit errorOccurred(ctx, QStringLiteral("OpenAI Images: invalid JSON response"));
            return;
        }
        const QJsonObject root = doc.object();
        const QJsonArray dataArr = root.value(QStringLiteral("data")).toArray();
        if (dataArr.isEmpty()) {
            const QString apiErr = root.value(QStringLiteral("error"))
                                       .toObject()
                                       .value(QStringLiteral("message"))
                                       .toString(QStringLiteral("OpenAI Images: empty data array"));
            emit errorOccurred(ctx, apiErr);
            return;
        }
        const QJsonObject first = dataArr.first().toObject();
        emit progressUpdate(ctx, 70);

        QByteArray imageBytes;
        const QString b64 = first.value(QStringLiteral("b64_json")).toString();
        if (!b64.isEmpty()) {
            imageBytes = QByteArray::fromBase64(b64.toUtf8());
        } else {
            const QString imageUrl = first.value(QStringLiteral("url")).toString();
            if (imageUrl.isEmpty()) {
                emit errorOccurred(
                    ctx, QStringLiteral("OpenAI Images: response missing both b64_json and url"));
                return;
            }
            QEventLoop dlLoop;
            QNetworkReply* dl = nam.get(QNetworkRequest(QUrl(imageUrl)));
            QObject::connect(dl, &QNetworkReply::finished, &dlLoop, &QEventLoop::quit);
            dlLoop.exec();
            if (dl->error() != QNetworkReply::NoError) {
                const QString msg = dl->errorString();
                dl->deleteLater();
                emit errorOccurred(ctx, QStringLiteral("OpenAI Images download: ") + msg);
                return;
            }
            imageBytes = dl->readAll();
            dl->deleteLater();
        }
        if (imageBytes.isEmpty()) {
            emit errorOccurred(ctx, QStringLiteral("OpenAI Images: decoded image is empty"));
            return;
        }
        const QString savePath = saveImageBytes(imageBytes);
        if (savePath.isEmpty()) {
            emit errorOccurred(ctx, QStringLiteral("OpenAI Images: cannot write image to disk"));
            return;
        }
        emit progressUpdate(ctx, 100);
        emit imageReady(ctx, savePath);
    }
}
