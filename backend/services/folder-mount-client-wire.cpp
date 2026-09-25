// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file folder-mount-client-wire.cpp
 * @brief Implementation of `FolderMountClientWire`. Each
 *        request blocks the caller on a heap-allocated wait state
 *        (`std::shared_ptr<WaitState>`) so a callback that fires
 *        late cannot dangle, dispatches via
 *        `WireHostBridge::dispatchClientRequest`, translates the
 *        reply into the typed return value.
 * @layer Service
 * @dependencies Qt6::Core, WireHostBridge, FolderMountRegistry,
 *               IFolderMountClient.
 */


#include "folder-mount-client-wire.h"

#include "../remote/wire-host-bridge.h"
#include "../utils/logger.h"
#include "folder-mount-registry.h"

#include <QtGlobal>
#include <QThread>

#include <algorithm>
#include <memory>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QSemaphore>

using Verzeta::Remote::BridgeClientRpcReply;
using Verzeta::Remote::WireHostBridge;

namespace {

/**
 * @brief Heap-allocated state shared between the caller's worker
 *        thread and the bridge callback. Ensures the reply and the
 *        semaphore remain valid even if the caller's stack frame
 *        unwinds before the bridge fires.
 */
struct WaitState {
    BridgeClientRpcReply reply;
    QSemaphore done{0};
};

/**
 * @brief Dispatches a wire RPC through the bridge and blocks the
 *        calling thread until the bridge callback fires or the
 *        safety margin expires. Returns the reply by value.
 */
BridgeClientRpcReply blockingDispatch(QPointer<WireHostBridge> bridge,
                                      const QString& clientId,
                                      const QString& op,
                                      const QJsonObject& args,
                                      int timeoutMs) {
    if (bridge.isNull()) {
        return BridgeClientRpcReply{false,
                                    QStringLiteral("bridge_offline"),
                                    QStringLiteral("wire host bridge has been torn down"),
                                    QJsonValue()};
    }

    auto state = std::make_shared<WaitState>();
    bridge->dispatchClientRequest(
        clientId,
        op,
        args,
        [state](const BridgeClientRpcReply& r) {
            state->reply = r;
            state->done.release();
        },
        timeoutMs);

    const int waitMs = timeoutMs + FolderMountClientWire::kSweepGraceMs;
    if (!state->done.tryAcquire(1, waitMs)) {
        // Should not happen — the bridge always fires the callback
        // exactly once (success / failure / timeout / drain). If it
        // doesn't, something is structurally wrong (sweep stuck,
        // memory corruption, bug). Synthesize a clean error so the
        // caller surfaces a refusal rather than blocking forever.
        return BridgeClientRpcReply{false,
                                    QStringLiteral("internal_error"),
                                    QStringLiteral("bridge callback never fired within budget"),
                                    QJsonValue()};
    }
    return state->reply;
}

}  // namespace

FolderMountClientWire::FolderMountClientWire(QPointer<WireHostBridge> bridge,
                                             FolderMountRegistry* registry)
    : m_bridge(bridge), m_registry(registry) {}

QString FolderMountClientWire::resolveClientId(const QString& folderId) {
    if (!m_registry)
        return {};

    QString clientId;
    auto fn = [this, &clientId, folderId]() {
        const auto mount = m_registry->mountForId(folderId);
        if (mount.has_value())
            clientId = mount->clientId;
    };

    if (QThread::currentThread() == m_registry->thread()) {
        // Same thread (typical test scenarios or future direct main-
        // thread callers). Direct call; no marshalling needed.
        fn();
    } else {
        // Cross-thread. ONE-WAY BlockingQueuedConnection (worker
        // waits for main; main never waits for worker). No deadlock
        // possible — mirrors the bridge's existing
        // QMetaObject::invokeMethod(Qt::BlockingQueuedConnection)
        // pattern for QML-property reads.
        QMetaObject::invokeMethod(m_registry, fn, Qt::BlockingQueuedConnection);
    }
    return clientId;
}

QByteArray FolderMountClientWire::readBytes(const QString& folderId,
                                            const QString& mountId,
                                            const QString& relPath,
                                            qint64 maxBytes,
                                            QString* outFingerprint,
                                            QString* outError) {
    if (outFingerprint)
        outFingerprint->clear();
    if (!outError) {
        qCWarning(verzetaUi) << "FolderMountClientWire::readBytes: outError nullptr — "
                                "caller cannot distinguish empty file from failure";
        return {};
    }
    outError->clear();

    const QString clientId = resolveClientId(folderId);
    if (clientId.isEmpty()) {
        *outError = QStringLiteral("mount_offline");
        return {};
    }

    const QJsonObject args{
        {QStringLiteral("folder_id"), folderId},
        {QStringLiteral("mount_id"), mountId},
        {QStringLiteral("rel_path"), relPath},
        {QStringLiteral("max_bytes"), static_cast<double>(maxBytes)},
    };

    const BridgeClientRpcReply reply =
        blockingDispatch(m_bridge, clientId, QStringLiteral("vfs.read"), args, kDefaultTimeoutMs);
    if (!reply.ok) {
        *outError = reply.errorKind;
        return {};
    }

    const QJsonObject data = reply.data.toObject();
    if (outFingerprint) {
        *outFingerprint = data.value(QStringLiteral("fingerprint")).toString();
    }
    const QString contentB64 = data.value(QStringLiteral("content")).toString();
    if (contentB64.isEmpty())
        return {};
    return QByteArray::fromBase64(contentB64.toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
}

qint64 FolderMountClientWire::writeBytes(const QString& folderId,
                                         const QString& mountId,
                                         const QString& relPath,
                                         const QByteArray& content,
                                         const QString& expectedFingerprint,
                                         QString* outError) {
    if (!outError) {
        qCWarning(verzetaUi) << "FolderMountClientWire::writeBytes: outError nullptr";
        return 0;
    }
    outError->clear();

    const QString clientId = resolveClientId(folderId);
    if (qEnvironmentVariableIntValue("VERZETA_MOUNT_TRACE") > 0) {
        qCInfo(verzetaUi).noquote()
            << "MOUNT-TRACE: writeBytes folder=" << folderId << "mount=" << mountId
            << "rel_path=" << relPath
            << "resolved_clientId=" << (clientId.isEmpty() ? QStringLiteral("<empty>") : clientId)
            << "content_bytes=" << content.size();
    }
    if (clientId.isEmpty()) {
        *outError = QStringLiteral("mount_offline");
        return 0;
    }

    QJsonObject args{
        {QStringLiteral("folder_id"), folderId},
        {QStringLiteral("mount_id"), mountId},
        {QStringLiteral("rel_path"), relPath},
        {QStringLiteral("content"), QString::fromLatin1(content.toBase64())},
    };
    if (!expectedFingerprint.isEmpty()) {
        args.insert(QStringLiteral("expected_fingerprint"), expectedFingerprint);
    }

    const BridgeClientRpcReply reply =
        blockingDispatch(m_bridge, clientId, QStringLiteral("vfs.write"), args, kDefaultTimeoutMs);
    if (qEnvironmentVariableIntValue("VERZETA_MOUNT_TRACE") > 0) {
        qCInfo(verzetaUi).noquote()
            << "MOUNT-TRACE: writeBytes reply ok=" << reply.ok << "errorKind="
            << (reply.errorKind.isEmpty() ? QStringLiteral("<empty>") : reply.errorKind);
    }
    if (!reply.ok) {
        *outError = reply.errorKind;
        return 0;
    }
    const QJsonObject data = reply.data.toObject();
    return static_cast<qint64>(
        data.value(QStringLiteral("applied_bytes")).toDouble(content.size()));
}

QJsonObject FolderMountClientWire::statPath(const QString& folderId,
                                            const QString& mountId,
                                            const QString& relPath,
                                            QString* outError) {
    if (!outError) {
        qCWarning(verzetaUi) << "FolderMountClientWire::statPath: outError nullptr";
        return {};
    }
    outError->clear();

    const QString clientId = resolveClientId(folderId);
    if (clientId.isEmpty()) {
        *outError = QStringLiteral("mount_offline");
        return {};
    }

    const QJsonObject args{
        {QStringLiteral("folder_id"), folderId},
        {QStringLiteral("mount_id"), mountId},
        {QStringLiteral("rel_path"), relPath},
    };

    const BridgeClientRpcReply reply =
        blockingDispatch(m_bridge, clientId, QStringLiteral("vfs.stat"), args, kDefaultTimeoutMs);
    if (!reply.ok) {
        *outError = reply.errorKind;
        return {};
    }
    return reply.data.toObject();
}

QJsonArray FolderMountClientWire::listDir(const QString& folderId,
                                          const QString& mountId,
                                          const QString& relPath,
                                          bool recursive,
                                          QString* outError) {
    if (!outError) {
        qCWarning(verzetaUi) << "FolderMountClientWire::listDir: outError nullptr";
        return {};
    }
    outError->clear();

    const QString clientId = resolveClientId(folderId);
    if (clientId.isEmpty()) {
        *outError = QStringLiteral("mount_offline");
        return {};
    }

    const QJsonObject args{
        {QStringLiteral("folder_id"), folderId},
        {QStringLiteral("mount_id"), mountId},
        {QStringLiteral("rel_path"), relPath},
        {QStringLiteral("recursive"), recursive},
    };

    const BridgeClientRpcReply reply =
        blockingDispatch(m_bridge, clientId, QStringLiteral("vfs.list"), args, kDefaultTimeoutMs);
    if (!reply.ok) {
        *outError = reply.errorKind;
        return {};
    }
    return reply.data.toObject().value(QStringLiteral("entries")).toArray();
}

QJsonObject FolderMountClientWire::executeCommand(const QString& folderId,
                                                  const QString& mountId,
                                                  const QString& conversationId,
                                                  const QString& command,
                                                  int timeoutMs,
                                                  qint64 maxOutputBytes,
                                                  QString* outError) {
    if (!outError) {
        qCWarning(verzetaUi) << "FolderMountClientWire::executeCommand: outError nullptr";
        return {};
    }
    outError->clear();

    const QString clientId = resolveClientId(folderId);
    if (clientId.isEmpty()) {
        *outError = QStringLiteral("mount_offline");
        return {};
    }

    // Clamp the deadline: never below 1 ms, never above the ceiling so a
    // hung command cannot pin this worker thread forever. The wire wait
    // (blockingDispatch) extends it by kSweepGraceMs internally.
    const int deadlineMs =
        timeoutMs <= 0 ? kExecDefaultTimeoutMs : std::min(timeoutMs, kExecMaxTimeoutMs);

    const QJsonObject args{
        {QStringLiteral("folder_id"), folderId},
        {QStringLiteral("mount_id"), mountId},
        {QStringLiteral("conversation_id"), conversationId},
        {QStringLiteral("command"), command},
        {QStringLiteral("timeout_ms"), deadlineMs},
        {QStringLiteral("max_output_bytes"), static_cast<double>(maxOutputBytes)},
    };

    const BridgeClientRpcReply reply =
        blockingDispatch(m_bridge, clientId, QStringLiteral("vfs.execute"), args, deadlineMs);
    if (!reply.ok) {
        *outError = reply.errorKind;
        return {};
    }
    return reply.data.toObject();
}
