// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file main.cpp
 * @brief Entry point of `verzeta-inference`, the standalone local-
 *        inference sidecar. Speaks the infer-protocol over stdio
 *        (length-prefixed JSON + binary frames), announces itself with
 *        a `hello` frame, and exits cleanly when stdin reaches EOF
 *        (the host's shutdown signal). This skeleton implements the
 *        protocol loop, `ping` and `status`; the model slots (embed +
 *        ragp GGUFs via llama.cpp) respond with a structured
 *        not-loaded error until the slot implementations land.
 * @layer Tool (separate-process sidecar)
 * @dependencies Qt6::Core only (JSON/byte handling). Deliberately NO
 *               event loop: a blocking exact-read stdio loop keeps
 *               the process model trivial and portable. stderr is the
 *               log channel (the host pipes it into its logger).
 */

#include <cstdio>
#include <cstring>
#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#else
#include <poll.h>
#include <unistd.h>
#endif

#include "embed-slot.h"
#include "infer-protocol.h"
#include "ragp-slot.h"

#ifdef VERZETA_INFER_HAS_LLAMA
#include <llama.h>
#endif

using Verzeta::Infer::decodeInferPayload;
using Verzeta::Infer::EmbedSlot;
using Verzeta::Infer::encodeInferFrame;
using Verzeta::Infer::InferFrame;
using Verzeta::Infer::kInferProtocolVersion;
using Verzeta::Infer::kMaxInferFrameBytes;
using Verzeta::Infer::RagpSlot;

namespace {

/**
 * @brief Writes one encoded frame to stdout and flushes immediately.
 *        Per-frame flush is mandatory: stdio buffering would otherwise
 *        hold a response hostage until the buffer fills (and on
 *        Windows text-mode translation would corrupt the binary
 *        stream; see the _setmode calls in main()).
 * @param header JSON header of the frame.
 * @param blob   Optional binary payload.
 * @returns True when fully written; false on write failure or when
 *          the frame exceeds the protocol cap.
 */
bool writeFrame(const QJsonObject& header, const QByteArray& blob = QByteArray()) {
    const QByteArray frame = encodeInferFrame(header, blob);
    if (frame.isEmpty()) {
        std::fprintf(stderr, "verzeta-inference: refusing oversized frame\n");
        return false;
    }
    const size_t n = std::fwrite(frame.constData(), 1, static_cast<size_t>(frame.size()), stdout);
    if (n != static_cast<size_t>(frame.size()))
        return false;
    std::fflush(stdout);
    return true;
}


/** @brief Builds the base of a response header echoing the request id. */
QJsonObject responseHeader(const char* op, const QJsonObject& request) {
    QJsonObject h;
    h.insert(QStringLiteral("op"), QLatin1String(op));
    if (request.contains(QStringLiteral("id"))) {
        h.insert(QStringLiteral("id"), request.value(QStringLiteral("id")));
    }
    return h;
}

/** @brief Structured failure response with a caller-supplied reason. */
QJsonObject errorResponse(const QJsonObject& request, const QString& reason) {
    QJsonObject h = responseHeader("error", request);
    h.insert(QStringLiteral("ok"), false);
    h.insert(QStringLiteral("error"), reason);
    return h;
}

/** The resident embed slot. Loaded/unloaded exclusively through the
 *  frame loop below (single-threaded, per the per-slot FIFO contract). */
EmbedSlot g_embedSlot;

/** The resident RAGP completion slot (same access contract). */
RagpSlot g_ragpSlot;

/** The resident CHAT completion slot (the local chat provider's
 *  model; same RagpSlot mechanics with streaming + temperature). */
RagpSlot g_chatSlot;

/** Buffered stdin bytes not yet consumed as frames. Filled by
 *  fillFromStdin(); drained by tryExtractFrame(). */
QByteArray g_inBuf;

/** Set once stdin reaches EOF. The main loop exits cleanly and an
 *  in-flight stream treats it as a cancel. */
bool g_stdinEof = false;

/**
 * @brief Pulls bytes from stdin into g_inBuf.
 * @param blocking True: block until at least one byte or EOF.
 *                 False: only read what is already available (the
 *                 mid-stream cancel poll, which never blocks generation).
 * @returns False on EOF (g_stdinEof latched), true otherwise.
 */
bool fillFromStdin(bool blocking) {
    if (g_stdinEof)
        return false;
    char tmp[4096];
#ifdef Q_OS_WIN
    if (!blocking) {
        DWORD avail = 0;
        if (!PeekNamedPipe(GetStdHandle(STD_INPUT_HANDLE), nullptr, 0, nullptr, &avail, nullptr) ||
            avail == 0) {
            return true;  // nothing waiting; not EOF-detectable here
        }
    }
    const int n = _read(_fileno(stdin), tmp, static_cast<unsigned int>(sizeof(tmp)));
#else
    if (!blocking) {
        struct pollfd pfd {
            0, POLLIN, 0
        };
        if (::poll(&pfd, 1, 0) <= 0 || !(pfd.revents & (POLLIN | POLLHUP))) {
            return true;  // nothing waiting
        }
    }
    const ssize_t n = ::read(0, tmp, sizeof(tmp));
#endif
    if (n <= 0) {
        g_stdinEof = true;
        return false;
    }
    g_inBuf.append(tmp, static_cast<qsizetype>(n));
    return true;
}

/**
 * @brief Extracts one complete frame from g_inBuf when available.
 * @param out      Decoded frame (valid only on true).
 * @param protoErr Set true on an unrecoverable framing error
 *                 (oversized length); the process must exit.
 * @returns True when a frame was extracted.
 */
bool tryExtractFrame(InferFrame* out, bool* protoErr) {
    if (g_inBuf.size() < 4)
        return false;
    const quint32 frameLen =
        (static_cast<quint8>(g_inBuf[0]) << 24) | (static_cast<quint8>(g_inBuf[1]) << 16) |
        (static_cast<quint8>(g_inBuf[2]) << 8) | static_cast<quint8>(g_inBuf[3]);
    if (static_cast<qint64>(frameLen) > kMaxInferFrameBytes || frameLen < 4) {
        if (protoErr)
            *protoErr = true;
        return false;
    }
    if (g_inBuf.size() < static_cast<qsizetype>(4 + frameLen)) {
        return false;
    }
    const QByteArray payload = g_inBuf.mid(4, frameLen);
    g_inBuf.remove(0, 4 + static_cast<qsizetype>(frameLen));
    if (!decodeInferPayload(payload, out)) {
        std::fprintf(stderr, "verzeta-inference: malformed frame skipped\n");
        return tryExtractFrame(out, protoErr);  // stream stays aligned
    }
    return true;
}

/**
 * @brief Mid-stream poll: consumes any frames already on stdin and
 *        reports whether generation of @p streamId must stop.
 *        A cancel for this stream (or stdin EOF) stops it; any other
 *        request mid-stream is answered with a structured busy error
 *        (the host serializes, so this is defensive only).
 * @param streamId The in-flight complete_stream request id.
 * @returns True when generation must stop.
 */
bool pollCancelOrEof(qint64 streamId) {
    if (!fillFromStdin(false))
        return true;  // EOF
    InferFrame f;
    bool protoErr = false;
    while (tryExtractFrame(&f, &protoErr)) {
        const QString op = f.header.value(QStringLiteral("op")).toString();
        const qint64 id = f.header.value(QStringLiteral("id")).toVariant().toLongLong();
        if (op == QLatin1String("cancel")) {
            if (id == streamId)
                return true;
            continue;  // stale cancel for a finished stream — ignore
        }
        QJsonObject h;
        h.insert(QStringLiteral("op"), QStringLiteral("error"));
        h.insert(QStringLiteral("id"), id);
        h.insert(QStringLiteral("ok"), false);
        h.insert(QStringLiteral("error"), QStringLiteral("busy streaming"));
        writeFrame(h);
    }
    return protoErr;
}

/** @brief The slot map reported by hello/status: loaded state, model
 *         id, and dimension per slot ("ragp" appears once it lands). */
QJsonObject slotsStatus() {
    QJsonObject embed;
    embed.insert(QStringLiteral("loaded"), g_embedSlot.isLoaded());
    if (g_embedSlot.isLoaded()) {
        embed.insert(QStringLiteral("model"), g_embedSlot.modelId());
        embed.insert(QStringLiteral("dim"), g_embedSlot.dim());
    }
    QJsonObject ragp;
    ragp.insert(QStringLiteral("loaded"), g_ragpSlot.isLoaded());
    if (g_ragpSlot.isLoaded()) {
        ragp.insert(QStringLiteral("model"), g_ragpSlot.modelId());
    }
    QJsonObject chat;
    chat.insert(QStringLiteral("loaded"), g_chatSlot.isLoaded());
    if (g_chatSlot.isLoaded()) {
        chat.insert(QStringLiteral("model"), g_chatSlot.modelId());
    }
    // NOTE: not named "slots" — that identifier is a Qt macro keyword.
    QJsonObject slotMap;
    slotMap.insert(QStringLiteral("embed"), embed);
    slotMap.insert(QStringLiteral("ragp"), ragp);
    slotMap.insert(QStringLiteral("chat"), chat);
    return slotMap;
}

/**
 * @brief Dispatches one decoded request frame; writes the response.
 * @param frame The decoded request.
 * @returns True to continue the loop; false to stop (write failure).
 */
bool dispatch(const InferFrame& frame) {
    const QString op = frame.header.value(QStringLiteral("op")).toString();

    if (op == QLatin1String("ping")) {
        return writeFrame(responseHeader("pong", frame.header));
    }
    if (op == QLatin1String("status")) {
        QJsonObject h = responseHeader("status_result", frame.header);
        h.insert(QStringLiteral("ok"), true);
        h.insert(QStringLiteral("proto"), kInferProtocolVersion);
        h.insert(QStringLiteral("slots"), slotsStatus());
        return writeFrame(h);
    }
    if (op == QLatin1String("load_model")) {
        const QString slot = frame.header.value(QStringLiteral("slot")).toString();
        const QString path = frame.header.value(QStringLiteral("path")).toString();
        if (slot == QLatin1String("embed")) {
            QString err;
            if (!g_embedSlot.load(path, &err)) {
                return writeFrame(errorResponse(frame.header, err));
            }
            QJsonObject h = responseHeader("load_result", frame.header);
            h.insert(QStringLiteral("ok"), true);
            h.insert(QStringLiteral("slot"), slot);
            h.insert(QStringLiteral("model"), g_embedSlot.modelId());
            h.insert(QStringLiteral("dim"), g_embedSlot.dim());
            h.insert(QStringLiteral("accel"), g_embedSlot.accel());
            std::fprintf(stderr,
                         "verzeta-inference: embed slot loaded (%s, dim %d)\n",
                         qPrintable(g_embedSlot.modelId()),
                         g_embedSlot.dim());
            return writeFrame(h);
        }
        if (slot == QLatin1String("chat")) {
            const int nCtx = frame.header.value(QStringLiteral("n_ctx")).toInt(8192);
            QString err;
            if (!g_chatSlot.load(path, &err, nCtx)) {
                return writeFrame(errorResponse(frame.header, err));
            }
            QJsonObject h = responseHeader("load_result", frame.header);
            h.insert(QStringLiteral("ok"), true);
            h.insert(QStringLiteral("slot"), slot);
            h.insert(QStringLiteral("model"), g_chatSlot.modelId());
            h.insert(QStringLiteral("n_ctx"), nCtx);
            h.insert(QStringLiteral("accel"), g_chatSlot.accel());
            std::fprintf(stderr,
                         "verzeta-inference: chat slot loaded (%s, ctx %d)\n",
                         qPrintable(g_chatSlot.modelId()),
                         nCtx);
            return writeFrame(h);
        }
        if (slot == QLatin1String("ragp")) {
            QString err;
            if (!g_ragpSlot.load(path, &err)) {
                return writeFrame(errorResponse(frame.header, err));
            }
            QJsonObject h = responseHeader("load_result", frame.header);
            h.insert(QStringLiteral("ok"), true);
            h.insert(QStringLiteral("slot"), slot);
            h.insert(QStringLiteral("model"), g_ragpSlot.modelId());
            h.insert(QStringLiteral("accel"), g_ragpSlot.accel());
            std::fprintf(stderr,
                         "verzeta-inference: ragp slot loaded (%s)\n",
                         qPrintable(g_ragpSlot.modelId()));
            return writeFrame(h);
        }
        return writeFrame(
            errorResponse(frame.header, QStringLiteral("unsupported slot: %1").arg(slot)));
    }
    if (op == QLatin1String("unload")) {
        const QString slot = frame.header.value(QStringLiteral("slot")).toString();
        if (slot == QLatin1String("embed"))
            g_embedSlot.unload();
        if (slot == QLatin1String("ragp"))
            g_ragpSlot.unload();
        if (slot == QLatin1String("chat"))
            g_chatSlot.unload();
        QJsonObject h = responseHeader("unload_result", frame.header);
        h.insert(QStringLiteral("ok"), true);  // idempotent
        h.insert(QStringLiteral("slot"), slot);
        return writeFrame(h);
    }
    if (op == QLatin1String("embed")) {
        if (!g_embedSlot.isLoaded()) {
            return writeFrame(errorResponse(frame.header, QStringLiteral("slot not loaded")));
        }
        QStringList texts;
        const QJsonArray arr = frame.header.value(QStringLiteral("texts")).toArray();
        texts.reserve(arr.size());
        for (const QJsonValue& v : arr)
            texts << v.toString();
        if (texts.isEmpty()) {
            return writeFrame(errorResponse(frame.header, QStringLiteral("no texts")));
        }
        QByteArray blob;
        QString err;
        if (!g_embedSlot.embed(texts, &blob, &err)) {
            return writeFrame(errorResponse(frame.header, err));
        }
        QJsonObject h = responseHeader("embed_result", frame.header);
        h.insert(QStringLiteral("ok"), true);
        h.insert(QStringLiteral("dim"), g_embedSlot.dim());
        h.insert(QStringLiteral("count"), static_cast<int>(texts.size()));
        h.insert(QStringLiteral("model"), g_embedSlot.modelId());
        return writeFrame(h, blob);
    }
    if (op == QLatin1String("complete_stream")) {
        if (!g_chatSlot.isLoaded()) {
            return writeFrame(errorResponse(frame.header, QStringLiteral("slot not loaded")));
        }
        const QString prompt = frame.header.value(QStringLiteral("prompt")).toString();
        if (prompt.isEmpty()) {
            return writeFrame(errorResponse(frame.header, QStringLiteral("empty prompt")));
        }
        const qint64 streamId = frame.header.value(QStringLiteral("id")).toVariant().toLongLong();
        const int maxTokens = frame.header.value(QStringLiteral("max_tokens")).toInt(1024);
        const double temperature = frame.header.value(QStringLiteral("temperature")).toDouble(0.7);

        int pieces = 0;
        bool writeOk = true;
        const auto onPiece = [&](const QString& delta) {
            if (!writeOk)
                return;
            QJsonObject c;
            c.insert(QStringLiteral("op"), QStringLiteral("chunk"));
            c.insert(QStringLiteral("id"), streamId);
            c.insert(QStringLiteral("delta"), delta);
            writeOk = writeFrame(c);
            ++pieces;
        };
        const auto shouldCancel = [&]() { return !writeOk || pollCancelOrEof(streamId); };
        QString finish;
        QString err;
        if (!g_chatSlot.completeStream(
                prompt, maxTokens, temperature, onPiece, shouldCancel, &finish, &err)) {
            return writeFrame(errorResponse(frame.header, err));
        }
        if (g_stdinEof)
            return false;  // host gone — clean exit path
        QJsonObject h = responseHeader("complete_result", frame.header);
        h.insert(QStringLiteral("ok"), true);
        h.insert(QStringLiteral("finish"), finish);
        h.insert(QStringLiteral("tokens"), pieces);
        h.insert(QStringLiteral("model"), g_chatSlot.modelId());
        return writeFrame(h);
    }
    if (op == QLatin1String("cancel")) {
        // A cancel arriving OUTSIDE a live stream targets a stream
        // that already finished — acknowledge-by-ignoring (the host
        // treats its own bookkeeping as authoritative).
        return true;
    }
    if (op == QLatin1String("complete")) {
        if (!g_ragpSlot.isLoaded()) {
            return writeFrame(errorResponse(frame.header, QStringLiteral("slot not loaded")));
        }
        const QString prompt = frame.header.value(QStringLiteral("prompt")).toString();
        if (prompt.isEmpty()) {
            return writeFrame(errorResponse(frame.header, QStringLiteral("empty prompt")));
        }
        const int maxTokens = frame.header.value(QStringLiteral("max_tokens")).toInt(256);
        const bool jsonStop = frame.header.value(QStringLiteral("json_stop")).toBool(false);
        // chat_template: wrap the prompt body in the loaded model's
        // own chat template (host prompt-builders send BODIES; only
        // the sidecar holds the model handle to template them).
        const bool chatTemplate = frame.header.value(QStringLiteral("chat_template")).toBool(false);
        QString text;
        QString err;
        if (!g_ragpSlot.complete(prompt, maxTokens, jsonStop, chatTemplate, &text, &err)) {
            return writeFrame(errorResponse(frame.header, err));
        }
        QJsonObject h = responseHeader("complete_result", frame.header);
        h.insert(QStringLiteral("ok"), true);
        h.insert(QStringLiteral("text"), text);
        h.insert(QStringLiteral("model"), g_ragpSlot.modelId());
        return writeFrame(h);
    }

    QJsonObject h = responseHeader("error", frame.header);
    h.insert(QStringLiteral("ok"), false);
    h.insert(QStringLiteral("error"), QStringLiteral("unsupported op"));
    return writeFrame(h);
}

}  // namespace

/**
 * @brief Sidecar main: binary stdio setup → hello → blocking frame
 *        loop → clean exit on stdin EOF.
 * @returns 0 on clean EOF shutdown; 1 on protocol/write failure.
 */
int main(int, char**) {
#ifdef Q_OS_WIN
    // Binary mode on BOTH pipes — Windows text mode translates \n and
    // would corrupt length-prefixed binary frames.
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif

#ifdef VERZETA_INFER_HAS_LLAMA
    // One-time llama.cpp runtime init (the standalone process owns its
    // own backend lifetime; the app never calls this for us).
    llama_backend_init();
#endif

    QJsonObject hello;
    hello.insert(QStringLiteral("op"), QStringLiteral("hello"));
    hello.insert(QStringLiteral("proto"), kInferProtocolVersion);
    hello.insert(QStringLiteral("slots"), slotsStatus());
    if (!writeFrame(hello))
        return 1;
    std::fprintf(stderr, "verzeta-inference: ready (proto %d)\n", kInferProtocolVersion);

    while (true) {
        InferFrame frame;
        bool protoErr = false;
        while (!tryExtractFrame(&frame, &protoErr)) {
            if (protoErr) {
                std::fprintf(stderr, "verzeta-inference: bad frame length\n");
                return 1;  // stream integrity gone — host restarts us
            }
            if (!fillFromStdin(true)) {
                std::fprintf(stderr, "verzeta-inference: stdin EOF — exiting\n");
                return 0;  // the host's polite shutdown
            }
        }
        if (!dispatch(frame)) {
            return g_stdinEof ? 0 : 1;
        }
    }
}
