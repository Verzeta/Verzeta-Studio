// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file infer-protocol.cpp
 * @brief Implementation of the host ↔ verzeta-inference frame codec
 *        (encode + payload decode; the incremental reader is header-
 *        only in infer-protocol.h).
 * @layer Utility (Inference IPC)
 * @dependencies Qt6::Core (QJsonDocument).
 */

#include "infer-protocol.h"

#include <QJsonDocument>

namespace Verzeta::Infer {

namespace {

/** @brief Appends one u32 in big-endian byte order. */
void appendU32BE(QByteArray& out, quint32 v) {
    out.append(static_cast<char>((v >> 24) & 0xff));
    out.append(static_cast<char>((v >> 16) & 0xff));
    out.append(static_cast<char>((v >> 8) & 0xff));
    out.append(static_cast<char>(v & 0xff));
}

/** @brief Reads one u32 in big-endian byte order at @p off. */
quint32 readU32BE(const QByteArray& buf, int off) {
    return (static_cast<quint8>(buf[off]) << 24) | (static_cast<quint8>(buf[off + 1]) << 16) |
           (static_cast<quint8>(buf[off + 2]) << 8) | static_cast<quint8>(buf[off + 3]);
}

}  // namespace

QByteArray encodeInferFrame(const QJsonObject& header, const QByteArray& blob) {
    const QByteArray json = QJsonDocument(header).toJson(QJsonDocument::Compact);
    const qint64 frameLen = 4LL + json.size() + blob.size();
    if (frameLen > kMaxInferFrameBytes) {
        return QByteArray();  // refusal — caller must not send
    }
    QByteArray out;
    out.reserve(static_cast<qsizetype>(4 + frameLen));
    appendU32BE(out, static_cast<quint32>(frameLen));
    appendU32BE(out, static_cast<quint32>(json.size()));
    out.append(json);
    out.append(blob);
    return out;
}

bool decodeInferPayload(const QByteArray& payload, InferFrame* out) {
    if (!out)
        return false;
    if (payload.size() < 4)
        return false;
    const quint32 jsonLen = readU32BE(payload, 0);
    if (static_cast<qint64>(jsonLen) > payload.size() - 4)
        return false;
    QJsonParseError err{};
    const QJsonDocument doc =
        QJsonDocument::fromJson(payload.mid(4, static_cast<qsizetype>(jsonLen)), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return false;
    }
    out->header = doc.object();
    out->blob = payload.mid(4 + static_cast<qsizetype>(jsonLen));
    return true;
}

}  // namespace Verzeta::Infer
