// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file canvas-console-model.cpp
 * @brief Implementation of CanvasConsoleModel. See header for design.
 * @layer Service (Model)
 * @dependencies Qt6::Core.
 */

#include "canvas-console-model.h"

#include <QDateTime>

CanvasConsoleModel::CanvasConsoleModel(QObject* parent) : QAbstractListModel(parent) {}

int CanvasConsoleModel::rowCount(const QModelIndex& parent) const {
    if (parent.isValid())
        return 0;
    return static_cast<int>(m_rows.size());
}

QVariant CanvasConsoleModel::data(const QModelIndex& index, int role) const {
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(m_rows.size())) {
        return {};
    }
    const Row& r = m_rows.at(index.row());
    switch (role) {
        case KindRole:
            return r.kind;
        case TextRole:
            return r.text;
        case TimestampRole:
            return r.timestamp;
        default:
            return {};
    }
}

QHash<int, QByteArray> CanvasConsoleModel::roleNames() const {
    return {
        {KindRole, QByteArrayLiteral("kind")},
        {TextRole, QByteArrayLiteral("text")},
        {TimestampRole, QByteArrayLiteral("timestamp")},
    };
}

int CanvasConsoleModel::count() const {
    return static_cast<int>(m_rows.size());
}

void CanvasConsoleModel::clear() {
    if (m_rows.isEmpty())
        return;
    beginResetModel();
    m_rows.clear();
    endResetModel();
    emit countChanged();
}

// ---------------------------------------------------------------------------
// Slots — the runner pushes events here.
// ---------------------------------------------------------------------------

void CanvasConsoleModel::onRunStarted(const QString& filename) {
    appendRow(QStringLiteral("system"),
              QStringLiteral("▶ Running %1")
                  .arg(filename.isEmpty() ? QStringLiteral("canvas") : filename));
}

void CanvasConsoleModel::onStdoutChunk(const QString& line) {
    appendRow(QStringLiteral("stdout"), trimLine(line));
}

void CanvasConsoleModel::onStderrChunk(const QString& line) {
    appendRow(QStringLiteral("stderr"), trimLine(line));
}

void CanvasConsoleModel::onRunFinished(int exitCode, qint64 elapsedMs) {
    // One concise summary row per the "minimal surface" mandate.
    QString text;
    QString kind;
    if (exitCode == 0) {
        kind = QStringLiteral("exit-ok");
        text = QStringLiteral("✓ Done (%1 ms)").arg(elapsedMs);
    } else if (exitCode == -1) {
        kind = QStringLiteral("exit-err");
        text = QStringLiteral("✗ Cancelled (%1 ms)").arg(elapsedMs);
    } else if (exitCode == -2) {
        kind = QStringLiteral("exit-err");
        text = QStringLiteral("✗ Timed out (%1 ms)").arg(elapsedMs);
    } else if (exitCode == -3) {
        kind = QStringLiteral("exit-err");
        text = QStringLiteral("✗ Sandbox unavailable");
    } else {
        kind = QStringLiteral("exit-err");
        text = QStringLiteral("✗ Exit %1 (%2 ms)").arg(exitCode).arg(elapsedMs);
    }
    appendRow(kind, text);
}

void CanvasConsoleModel::onRunFailedToStart(const QString& reason) {
    appendRow(QStringLiteral("exit-err"),
              QStringLiteral("✗ Failed to start: %1").arg(trimLine(reason)));
}

void CanvasConsoleModel::onInputEcho(const QString& text) {
    // "> " marks the row as a line the USER typed, mirroring a terminal, so the
    // console reads as a prompt/answer transcript.
    appendRow(QStringLiteral("input"), QStringLiteral("> %1").arg(trimLine(text)));
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

void CanvasConsoleModel::appendRow(const QString& kind, const QString& text) {
    // FIFO trim — drop oldest rows before appending if we'd exceed kRowCap.
    while (m_rows.size() >= kRowCap) {
        beginRemoveRows({}, 0, 0);
        m_rows.removeFirst();
        endRemoveRows();
    }

    const int row = m_rows.size();
    beginInsertRows({}, row, row);
    m_rows.append(Row{kind, text, nowStamp()});
    endInsertRows();
    emit countChanged();
    emit lineAppended(kind, text);
}

QString CanvasConsoleModel::trimLine(const QString& s) {
    if (s.size() <= kMaxLineChars)
        return s;
    return s.left(kMaxLineChars) + QStringLiteral("…");
}

QString CanvasConsoleModel::nowStamp() {
    return QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
}
