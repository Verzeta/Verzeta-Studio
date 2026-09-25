// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file infer-protocol.h
 * @brief Shared frame codec for the host ↔ `verzeta-inference` sidecar
 *        stdio IPC. One frame carries a JSON header plus an optional
 *        raw binary blob (embedding responses ship float32 vectors as
 *        the blob, never as JSON-encoded numbers). Compiled into BOTH the
 *        main backend (host side) and the standalone sidecar binary so
 *        the two ends can never drift.
 * @layer Utility (Inference IPC)
 * @dependencies Qt6::Core only (QByteArray, QJsonObject). No sockets,
 *               no event loop. Pure byte-level encode/decode usable
 *               from a blocking stdio loop or a QProcess reader alike.
 *
 * ## Wire format (all integers big-endian)
 *   [u32 frameLen][u32 jsonLen][json bytes][blob bytes]
 * where frameLen = 4 + jsonLen + blob size and frameLen is capped at
 * kMaxInferFrameBytes. A frame with no blob has
 * frameLen == 4 + jsonLen.
 *
 * ## Protocol v1 ops (JSON header field "op")
 *   host → sidecar : ping, status, load_model, unload, embed, complete
 *   sidecar → host : hello (once on start), pong, status_result,
 *                    load_result, embed_result (+ float32 blob),
 *                    complete_result, error
 * Every request carries a numeric "id" the response echoes. The
 * sidecar exits cleanly when stdin reaches EOF, which is the host's
 * shutdown signal (no kill needed on the happy path).
 */

#pragma once

#include <QByteArray>
#include <QJsonObject>

namespace Verzeta::Infer {

/** Protocol version announced in the sidecar's hello frame. */
inline constexpr int kInferProtocolVersion = 1;

/** Hard cap on one frame's frameLen. Mirrors the wire subsystem's
 *  24 MiB frame cap so any payload the app accepts elsewhere also fits
 *  here; oversized frames are a protocol error (connection dropped). */
inline constexpr qint64 kMaxInferFrameBytes = 24LL * 1024 * 1024;

/**
 * @brief One decoded frame: the JSON header and the optional blob.
 */
struct InferFrame {
    QJsonObject header;  ///< Parsed JSON header (op, id, …)
    QByteArray blob;     ///< Raw binary payload; empty when absent
};

/**
 * @brief Encodes a header (+ optional blob) into the wire format.
 * @param header JSON header object (must at least carry "op").
 * @param blob   Optional raw binary payload appended after the JSON.
 * @returns The complete frame bytes (prefix included), or an EMPTY
 *          array when the encoded frame would exceed
 *          kMaxInferFrameBytes (callers treat empty as refusal).
 */
QByteArray encodeInferFrame(const QJsonObject& header, const QByteArray& blob = QByteArray());

/**
 * @brief Decodes one frame PAYLOAD (the bytes AFTER the u32 frameLen
 *        prefix) into header + blob.
 * @param payload  Exactly frameLen bytes as read from the stream.
 * @param out      Decoded frame (valid only when true is returned).
 * @returns True on success; false when the payload is truncated,
 *          jsonLen is inconsistent, or the JSON does not parse to an
 *          object.
 */
bool decodeInferPayload(const QByteArray& payload, InferFrame* out);

/**
 * @brief Incremental stream splitter for the host side: append bytes
 *        as they arrive from the sidecar's stdout and drain every
 *        complete frame. Partial frames stay buffered.
 *
 * Mirrors the wire FrameReader idiom; templated emit keeps it free of
 * signal machinery so the sidecar's tests can reuse it too.
 */
class InferFrameReader {
  public:
    /**
     * @brief Appends bytes and invokes @p emitFrame per decoded frame.
     * @param chunk     Newly-read bytes.
     * @param emitFrame Callable taking (const InferFrame&).
     * @returns True to keep reading; false on an oversized frameLen
     *          (protocol error; the caller must drop the peer).
     */
    template <typename Emit> bool feed(const QByteArray& chunk, Emit emitFrame) {
        m_buf.append(chunk);
        while (true) {
            if (m_buf.size() < 4)
                return true;
            const quint32 frameLen =
                (static_cast<quint8>(m_buf[0]) << 24) | (static_cast<quint8>(m_buf[1]) << 16) |
                (static_cast<quint8>(m_buf[2]) << 8) | static_cast<quint8>(m_buf[3]);
            if (static_cast<qint64>(frameLen) > kMaxInferFrameBytes) {
                return false;
            }
            if (m_buf.size() < static_cast<qsizetype>(4 + frameLen)) {
                return true;
            }
            const QByteArray payload = m_buf.mid(4, frameLen);
            m_buf.remove(0, 4 + static_cast<qsizetype>(frameLen));
            InferFrame frame;
            if (decodeInferPayload(payload, &frame)) {
                emitFrame(frame);
            }
            // Malformed-but-bounded payloads are skipped (stream stays
            // aligned because the length prefix was honoured).
        }
    }

    /** @brief Drops any buffered partial frame. */
    void clear() { m_buf.clear(); }

  private:
    QByteArray m_buf;
};

}  // namespace Verzeta::Infer
