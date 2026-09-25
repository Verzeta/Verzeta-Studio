// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file export-service.h
 * @brief Exports conversations to Markdown or JSON format files.
 *        Reads conversation metadata and all messages (including attachments
 *        and tool calls) from the service layer and writes structured files.
 * @layer Service
 * @dependencies ConversationService (Service), MessageService (Service),
 *               Qt6::Core
 */


#pragma once

#include <QJsonObject>
#include <QObject>
#include <QString>

// Forward declarations
class ConversationService;
class MessageService;
struct Conversation;
struct Message;

/**
 * @brief Service that exports a conversation to a file on disk.
 *
 * Two formats are supported:
 *   - Markdown (.md): Human-readable, suitable for sharing and archiving.
 *   - JSON (.json):   Machine-parseable, suitable for import, analysis, backup.
 *
 * Both formats include:
 *   - Conversation metadata (title, model, system prompt, token total)
 *   - All messages in chronological order
 *   - Tool call records (name, arguments, result)
 *   - Attachment file names (base64 data only when includeAttachments=true)
 *
 * Usage:
 * @code
 *   ExportService exporter(convSvc, msgSvc, this);
 *   connect(&exporter, &ExportService::exportCompleted, ...);
 *   connect(&exporter, &ExportService::exportFailed,    ...);
 *   exporter.exportToMarkdown(convId, "/tmp/chat.md");
 * @endcode
 *
 * Thread safety: Designed for main-thread use. File I/O is synchronous.
 */
class ExportService : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Constructs the ExportService.
     * @param convSvc Reference to ConversationService for conversation metadata.
     * @param msgSvc  Reference to MessageService for message retrieval.
     * @param parent  Optional Qt parent.
     */
    explicit ExportService(ConversationService& convSvc,
                           MessageService& msgSvc,
                           QObject* parent = nullptr);

    // -----------------------------------------------------------------------
    // Public export methods
    // -----------------------------------------------------------------------

    /**
     * @brief Exports a conversation to a Markdown file.
     *
     * Output format:
     * @code
     *   # {title}
     *   *Exported: {ISO} | Model: {model} | Total Tokens: {count}*
     *
     *   **System Prompt:**
     *   > {system_prompt}
     *   ---
     *   **You** *(HH:MM:SS)*
     *   {content}
     *
     *   **Assistant** *(HH:MM:SS)* · {model_used}
     *   {content}
     *   ---
     *   *Exported by Verzeta Studio*
     * @endcode
     *
     * @param convId              UUID of the conversation to export.
     * @param destPath            Absolute file path to write (e.g., "/home/user/chat.md").
     * @param includeAttachments  If true, base64-encodes and embeds binary attachments.
     * @return true if the file was written successfully.
     * @sideeffects Writes file at destPath. Emits exportCompleted or exportFailed.
     */
    bool exportToMarkdown(const QString& convId,
                          const QString& destPath,
                          bool includeAttachments = false);

    /**
     * @brief Exports a conversation to a JSON file.
     *
     * Output format:
     * @code
     * {
     *   "version": "1.0",
     *   "conversation": { "id", "title", "created_at", "system_prompt", "llm_config" },
     *   "messages": [ { "id", "role", "content", "created_at", "token_count",
     *                   "model_used", "finish_reason", "metadata",
     *                   "attachments", "tool_calls" } ],
     *   "statistics": { "total_tokens", "total_messages", "total_cost_usd" }
     * }
     * @endcode
     *
     * @param convId              UUID of the conversation to export.
     * @param destPath            Absolute file path to write (e.g., "/home/user/chat.json").
     * @param includeAttachments  If true, base64-encodes and embeds binary attachments.
     * @return true if the file was written successfully.
     * @sideeffects Writes file at destPath. Emits exportCompleted or exportFailed.
     */
    bool
    exportToJson(const QString& convId, const QString& destPath, bool includeAttachments = false);

  signals:
    /**
     * @brief Emitted when an export completes successfully.
     * @param filePath The absolute path of the exported file.
     */
    void exportCompleted(const QString& filePath);

    /**
     * @brief Emitted when an export fails.
     * @param error Human-readable error description.
     */
    void exportFailed(const QString& error);

  private:
    ConversationService& m_convSvc;
    MessageService& m_msgSvc;

    /**
     * @brief Formats a single message as a Markdown snippet.
     * @param msg                The message to format.
     * @param includeAttachments Whether to embed attachment data.
     * @return Markdown string ending with a blank line.
     */
    QString formatMessageMarkdown(const Message& msg, bool includeAttachments) const;

    /**
     * @brief Formats a single message as a QJsonObject for JSON export.
     * @param msg                The message to format.
     * @param includeAttachments Whether to embed attachment base64 data.
     * @return QJsonObject with all message fields.
     */
    QJsonObject formatMessageJson(const Message& msg, bool includeAttachments) const;

    /**
     * @brief Writes content to a file at destPath, creating parent directories.
     * @param destPath  Absolute file path.
     * @param content   UTF-8 text content to write.
     * @return true if the file was written without error.
     * @sideeffects Creates or overwrites the file at destPath.
     */
    bool writeFile(const QString& destPath, const QString& content) const;
};
