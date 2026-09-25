// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file export-controller.cpp
 * @brief Implementation of the `Export` QML singleton. Owns the
 *        single exportConversation Q_INVOKABLE + the two result
 *        signals; delegates the actual file write to ExportService.
 * @layer Service
 * @dependencies ConversationService, ExportService (references).
 */

#include "export-controller.h"

#include "../models/conversation.h"
#include "../utils/logger.h"
#include "../utils/thread-discipline.h"
#include "conversation-service.h"
#include "export-service.h"

#include <QDateTime>
#include <QRegularExpression>
#include <QStandardPaths>

ExportController::ExportController(ConversationService& convSvc,
                                   ExportService& exportSvc,
                                   QObject* parent)
    : QObject(parent), m_convSvc(convSvc), m_exportSvc(exportSvc) {
    // Forward ExportService result signals onto our QML-facing
    // channels so QML never holds an ExportService reference.
    connect(
        &m_exportSvc, &ExportService::exportCompleted, this, &ExportController::exportCompleted);
    connect(&m_exportSvc, &ExportService::exportFailed, this, &ExportController::exportFailed);

    qCInfo(verzetaUi) << "ExportController initialized";
}

ExportController::~ExportController() {
    qCInfo(verzetaUi) << "ExportController destroyed";
}

void ExportController::exportConversation(const QString& convId,
                                          const QString& format,
                                          bool includeAttachments) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (convId.isEmpty()) {
        emit exportFailed(QStringLiteral("No conversation selected"));
        return;
    }

    const QString downloadsDir = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);

    const auto conv = m_convSvc.getConversation(convId);
    const QString title = conv.has_value() ? conv->title : QStringLiteral("conversation");

    QString safeTitle = title;
    safeTitle.replace(QRegularExpression(QStringLiteral("[^a-zA-Z0-9_\\-]")), QStringLiteral("_"));
    safeTitle = safeTitle.left(64);

    const QString timestamp =
        QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd_HHmmss"));

    QString destPath;
    if (format == QStringLiteral("json")) {
        destPath = QStringLiteral("%1/%2_%3.json").arg(downloadsDir, safeTitle, timestamp);
        m_exportSvc.exportToJson(convId, destPath, includeAttachments);
    } else {
        destPath = QStringLiteral("%1/%2_%3.md").arg(downloadsDir, safeTitle, timestamp);
        m_exportSvc.exportToMarkdown(convId, destPath, includeAttachments);
    }
}
