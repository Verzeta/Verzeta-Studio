// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file export-controller.h
 * @brief QML-exposed controller that owns conversation-export
 *        kickoff + result notification. Registered as the `Export`
 *        QML singleton by AppController, alongside `ChatController`,
 *        `Conversations`, `Tasks`, and `AgentSettings`.
 * @layer Service (UI orchestration)
 * @dependencies ConversationService, ExportService (non-owning refs).
 *
 * The controller carries the single `exportConversation` Q_INVOKABLE
 * + the two result signals (`exportCompleted`, `exportFailed`). The
 * actual file-writing work stays inside ExportService; this
 * controller is the QML-facing kickoff + destination-path computation
 * + signal forwarder.
 *
 * Star-topology refusal: ExportController does NOT hold a
 * ChatController pointer. Result signals come straight from
 * ExportService and are forwarded onto our own
 * exportCompleted / exportFailed signals via direct connect at
 * construction time.
 *
 * Threading: strictly main-thread. Every public method asserts via
 * VERZETA_ASSERT_MAIN_THREAD().
 *
 * Ownership: constructed as `std::unique_ptr<ExportController>` on
 * AppController. AppController is the QObject parent. QML holds a
 * non-owning singleton pointer via `qmlRegisterSingletonInstance`.
 */
#pragma once

#include <QObject>
#include <QString>

class ConversationService;
class ExportService;

/**
 * @brief QML singleton for conversation export.
 */
class ExportController : public QObject {
    Q_OBJECT

  public:
    /**
     * @param convSvc    Non-owning reference; resolves the
     *                   conversation title for the auto-generated
     *                   filename.
     * @param exportSvc  Non-owning reference; performs the actual
     *                   markdown / json file write and emits its
     *                   own exportCompleted / exportFailed signals
     *                   that this controller forwards.
     * @param parent     Qt parent (AppController).
     */
    explicit ExportController(ConversationService& convSvc,
                              ExportService& exportSvc,
                              QObject* parent = nullptr);
    ~ExportController() override;

    /**
     * @brief Exports the supplied conversation. The destination path
     *        is auto-generated from the conversation title and
     *        QStandardPaths::DownloadLocation. Result is delivered
     *        via the `exportCompleted(filePath)` /
     *        `exportFailed(error)` signals.
     * @param convId             UUID of the conversation to export.
     * @param format             "markdown" or "json".
     * @param includeAttachments Whether to embed binary attachment data.
     */
    Q_INVOKABLE void exportConversation(const QString& convId,
                                        const QString& format,
                                        bool includeAttachments = false);

  signals:
    /**
     * @brief Emitted on a successful file write. Forwarded from
     *        ExportService::exportCompleted.
     * @param filePath Absolute path of the exported file.
     */
    void exportCompleted(const QString& filePath);

    /**
     * @brief Emitted on failure. Forwarded from
     *        ExportService::exportFailed.
     * @param error Human-readable failure description.
     */
    void exportFailed(const QString& error);

  private:
    ConversationService& m_convSvc;
    ExportService& m_exportSvc;
};
