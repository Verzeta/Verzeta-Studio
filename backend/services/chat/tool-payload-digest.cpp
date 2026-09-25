// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file tool-payload-digest.cpp
 * @brief Implementation of Chat::ToolPayloadDigest.
 * @layer Service (Chat subsystem)
 * @dependencies Qt6::Core (QJsonObject / QJsonDocument / QJsonArray / QString).
 */

#include "tool-payload-digest.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QStringList>

namespace Chat {

namespace {

/// First present string value among @p keys, or empty.
QString firstString(const QJsonObject& obj, const QStringList& keys) {
    for (const QString& k : keys) {
        const QJsonValue v = obj.value(k);
        if (v.isString() && !v.toString().isEmpty()) {
            return v.toString();
        }
    }
    return QString();
}

/// Argument keys that name the target file/path across the file/canvas tools.
const QStringList& fileKeys() {
    static const QStringList k = {QStringLiteral("filename"),
                                  QStringLiteral("path"),
                                  QStringLiteral("file"),
                                  QStringLiteral("name")};
    return k;
}

/// Argument keys that carry the large body payload across the file/canvas tools.
const QStringList& contentKeys() {
    static const QStringList k = {QStringLiteral("content"),
                                  QStringLiteral("new_content"),
                                  QStringLiteral("text"),
                                  QStringLiteral("data")};
    return k;
}

}  // namespace

bool ToolPayloadDigest::isDigestedArgs(const QJsonObject& args) {
    return args.contains(QString::fromLatin1(kArgsDigestKey));
}

bool ToolPayloadDigest::isDigestedResult(const QString& result) {
    return result.startsWith(QString::fromLatin1(kResultDigestPrefix));
}

QJsonObject ToolPayloadDigest::digestArgs(const QString& toolName, const QJsonObject& args) {
    if (isDigestedArgs(args)) {
        return args;  // idempotent
    }

    const QString fileName = firstString(args, fileKeys());

    // Locate the body field (the large content payload), if any.
    QString contentKey;
    QString content;
    for (const QString& k : contentKeys()) {
        const QJsonValue v = args.value(k);
        if (v.isString()) {
            contentKey = k;
            content = v.toString();
            break;
        }
    }

    // Keep every non-body field verbatim; defensively elide any oversized
    // string field so an unknown tool's giant arg can't slip through.
    QJsonObject out;
    for (auto it = args.constBegin(); it != args.constEnd(); ++it) {
        if (!contentKey.isEmpty() && it.key() == contentKey) {
            continue;  // body stripped; summarized below
        }
        const QJsonValue v = it.value();
        if (v.isString() && v.toString().length() > kFieldElideThreshold) {
            out.insert(it.key(), QStringLiteral("[%1 chars elided]").arg(v.toString().length()));
        } else {
            out.insert(it.key(), v);
        }
    }

    QString summary;
    if (!content.isEmpty()) {
        const int lines = content.count(QLatin1Char('\n')) + 1;
        const int bytes = static_cast<int>(content.toUtf8().size());
        const QString label = fileName.isEmpty() ? QStringLiteral("content") : fileName;
        summary = QStringLiteral("%1 · %2 lines · %3 B "
                                 "(full content elided — read_file/read_canvas to view)")
                      .arg(label)
                      .arg(lines)
                      .arg(bytes);
    } else {
        summary = QStringLiteral("arguments elided — re-read with read_file/read_canvas");
    }
    Q_UNUSED(toolName);
    out.insert(QString::fromLatin1(kArgsDigestKey), summary);
    return out;
}

QString ToolPayloadDigest::digestResult(const QString& toolName, const QString& resultJson) {
    if (isDigestedResult(resultJson)) {
        return resultJson;  // idempotent
    }

    const QString prefix = QString::fromLatin1(kResultDigestPrefix);
    const QJsonObject obj = QJsonDocument::fromJson(resultJson.toUtf8()).object();

    if (toolName == QStringLiteral("search_web")) {
        const QString answer = firstString(obj, {QStringLiteral("answer")});
        QStringList sources;
        const QJsonArray results = obj.value(QStringLiteral("results")).toArray();
        for (const QJsonValue& rv : results) {
            if (sources.size() >= 5) {
                break;
            }
            const QJsonObject ro = rv.toObject();
            const QString title = ro.value(QStringLiteral("title")).toString();
            const QString url = ro.value(QStringLiteral("url")).toString();
            if (!title.isEmpty() || !url.isEmpty()) {
                sources.append(QStringLiteral("%1 (%2)").arg(title, url));
            }
        }
        QString out = prefix + QStringLiteral("search result");
        if (!answer.isEmpty()) {
            out += QStringLiteral(" · %1").arg(answer);
        }
        if (!sources.isEmpty()) {
            out += QStringLiteral(" · sources: %1").arg(sources.join(QStringLiteral("; ")));
        }
        out += QStringLiteral(" · re-run search_web for full results");
        return out;
    }

    if (toolName == QStringLiteral("read_file") || toolName == QStringLiteral("read_canvas") ||
        toolName == QStringLiteral("open_canvas")) {
        const QString path = firstString(
            obj, {QStringLiteral("path"), QStringLiteral("filename"), QStringLiteral("file")});
        QString out = prefix + QStringLiteral("read");
        if (!path.isEmpty()) {
            out += QStringLiteral(" %1").arg(path);
        }
        const QJsonValue startV = obj.value(QStringLiteral("start_line"));
        const QJsonValue endV = obj.value(QStringLiteral("end_line"));
        const QJsonValue totV = obj.value(QStringLiteral("total_lines"));
        if (startV.isDouble() && endV.isDouble()) {
            out += QStringLiteral(" lines %1–%2").arg(startV.toInt()).arg(endV.toInt());
        } else if (totV.isDouble()) {
            out += QStringLiteral(" (%1 lines)").arg(totV.toInt());
        }
        // Keep a LEADING SNIPPET of the fetched body, not a circular
        // "re-read" instruction — the model read this content to USE it,
        // so it must retain at least the head of it. Only truly old reads
        // reach here (recent ones stay verbatim via the protected window).
        QString body = firstString(obj, {QStringLiteral("content"), QStringLiteral("text")});
        if (body.isEmpty()) {
            body = resultJson;  // fallback when there is no content field
        }
        body = body.trimmed();
        if (body.length() > kReadSnippetChars) {
            out += QStringLiteral(" — %1… (truncated — re-read a specific line range via "
                                  "start_line/end_line for the full text)")
                       .arg(body.left(kReadSnippetChars));
        } else if (!body.isEmpty()) {
            out += QStringLiteral(" — %1").arg(body);
        }
        return out;
    }

    if (toolName == QStringLiteral("write_file") || toolName == QStringLiteral("edit_canvas")) {
        const QString path = firstString(obj, {QStringLiteral("path"), QStringLiteral("filename")});
        const QJsonValue bytesV = obj.value(QStringLiteral("bytes_written"));
        QString out = prefix + QStringLiteral("wrote");
        if (!path.isEmpty()) {
            out += QStringLiteral(" %1").arg(path);
        }
        if (bytesV.isDouble()) {
            out += QStringLiteral(" · %1 B").arg(bytesV.toInt());
        }
        return out;
    }

    // Generic: elide an oversized body; keep short results verbatim.
    if (resultJson.length() > kFieldElideThreshold) {
        const QString label = toolName.isEmpty() ? QStringLiteral("tool") : toolName;
        return prefix + QStringLiteral("%1 result · %2 chars elided — re-run to re-fetch")
                            .arg(label)
                            .arg(resultJson.length());
    }
    return resultJson;
}

QString ToolPayloadDigest::summarizeCall(const QString& toolName,
                                         const QJsonObject& args,
                                         const QString& resultJson) {
    const QString name = toolName.isEmpty() ? QStringLiteral("tool") : toolName;

    // --- Call side: name + target file + body size (content tools), or the
    //     small scalar args (query / mode / lines) for non-content tools. ---
    QString call = name;
    const QString fileName = firstString(args, fileKeys());
    if (!fileName.isEmpty()) {
        call += QStringLiteral(" ") + fileName;
    }

    QString body;
    for (const QString& k : contentKeys()) {
        const QJsonValue v = args.value(k);
        if (v.isString()) {
            body = v.toString();
            break;
        }
    }
    if (!body.isEmpty()) {
        const int lines = body.count(QLatin1Char('\n')) + 1;
        const int bytes = static_cast<int>(body.toUtf8().size());
        call += QStringLiteral(" (%1 lines, %2 B)").arg(lines).arg(bytes);
    } else {
        // Surface the small scalar args (query, mode, line range) so the
        // record still says WHAT was asked, never a body.
        QStringList small;
        for (auto it = args.constBegin(); it != args.constEnd(); ++it) {
            const QString key = it.key();
            if (key.startsWith(QStringLiteral("__"))  // host-injected
                || fileKeys().contains(key) || contentKeys().contains(key) ||
                key == QString::fromLatin1(kArgsDigestKey)) {
                continue;
            }
            const QJsonValue v = it.value();
            QString sv;
            if (v.isString()) {
                sv = v.toString();
            } else if (v.isDouble() || v.isBool()) {
                sv = v.toVariant().toString();
            } else {
                continue;
            }
            if (sv.length() > kFieldElideThreshold) {
                continue;
            }
            small.append(QStringLiteral("%1=%2").arg(key, sv));
        }
        if (!small.isEmpty()) {
            call += QStringLiteral(" {%1}").arg(small.join(QStringLiteral(", ")));
        }
    }

    // --- Result side: reuse digestResult, strip the "[digest] " prefix so the
    //     prose record reads naturally; keep it on one line. ---
    QString res = digestResult(name, resultJson);
    const QString pfx = QString::fromLatin1(kResultDigestPrefix);
    if (res.startsWith(pfx)) {
        res = res.mid(pfx.size());
    }
    res = res.trimmed();
    res.replace(QLatin1Char('\n'), QLatin1Char(' '));

    if (res.isEmpty()) {
        return call;
    }
    return call + QStringLiteral(" → ") + res;
}

}  // namespace Chat
