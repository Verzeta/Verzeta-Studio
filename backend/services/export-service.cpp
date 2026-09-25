// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file export-service.cpp
 * @brief Implementation of ExportService, which converts conversation data
 *        from the database into Markdown or JSON files on disk.
 * @layer Service
 * @dependencies ConversationService (Service), MessageService (Service),
 *               Qt6::Core
 */


#include "export-service.h"

#include "../models/attachment.h"
#include "../models/message.h"
#include "../models/tool-call.h"
#include "../services/conversation-service.h"
#include "../services/message-service.h"
#include "../utils/logger.h"
#include "../utils/thread-discipline.h"

#include <QTextStream>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

/*
 * @brief Constructs the ExportService.
 * @param convSvc Reference to ConversationService.
 * @param msgSvc  Reference to MessageService.
 * @param parent  Optional Qt parent.
 */
ExportService::ExportService(ConversationService& convSvc, MessageService& msgSvc, QObject* parent)
    : QObject(parent), m_convSvc(convSvc), m_msgSvc(msgSvc) {}

// ---------------------------------------------------------------------------
// Public export methods
// ---------------------------------------------------------------------------

/*
 * @brief Exports a conversation to a Markdown file.
 * @param convId              UUID of the conversation.
 * @param destPath            Absolute destination file path.
 * @param includeAttachments  Whether to embed base64 attachment data.
 * @return true if successfully written.
 * @sideeffects Writes file to disk. Emits exportCompleted or exportFailed.
 */
bool ExportService::exportToMarkdown(const QString& convId,
                                     const QString& destPath,
                                     bool includeAttachments) {
    VERZETA_ASSERT_MAIN_THREAD();
    // Load conversation metadata
    const auto convOpt = m_convSvc.getConversation(convId);
    if (!convOpt.has_value()) {
        const QString err = QStringLiteral("Conversation not found: ") + convId;
        qCWarning(verzetaUi) << "ExportService:" << err;
        emit exportFailed(err);
        return false;
    }
    const Conversation& conv = *convOpt;

    // Load all messages
    const QList<Message> messages = m_msgSvc.getMessages(convId);

    // Compute total tokens
    int totalTokens = 0;
    for (const Message& m : messages) {
        totalTokens += m.tokenCount;
    }

    // Determine model name from last assistant message or llm_config
    QString modelDisplay = QStringLiteral("-");
    for (int i = messages.size() - 1; i >= 0; --i) {
        if (messages[i].role == QStringLiteral("assistant") && !messages[i].modelUsed.isEmpty()) {
            modelDisplay = messages[i].modelUsed;
            break;
        }
    }

    // Build Markdown content
    QString md;
    QTextStream out(&md);

    out << QStringLiteral("# ") << conv.title << QStringLiteral("\n\n");
    out << QStringLiteral("*Exported: ") << QDateTime::currentDateTimeUtc().toString(Qt::ISODate)
        << QStringLiteral(" | Model: ") << modelDisplay << QStringLiteral(" | Total Tokens: ")
        << totalTokens << QStringLiteral("*\n\n");

    if (!conv.systemPrompt.isEmpty()) {
        out << QStringLiteral("**System Prompt:**\n");
        // Format each line of the system prompt as a blockquote
        const QStringList promptLines = conv.systemPrompt.split(QStringLiteral("\n"));
        for (const QString& line : promptLines) {
            out << QStringLiteral("> ") << line << QStringLiteral("\n");
        }
        out << QStringLiteral("\n");
    }

    out << QStringLiteral("---\n\n");

    for (const Message& msg : messages) {
        out << formatMessageMarkdown(msg, includeAttachments);
    }

    out << QStringLiteral("---\n*Exported by Verzeta Studio*\n");

    if (!writeFile(destPath, md)) {
        const QString err = QStringLiteral("Failed to write file: ") + destPath;
        emit exportFailed(err);
        return false;
    }

    qCInfo(verzetaUi) << "ExportService: Markdown export complete:" << destPath;
    emit exportCompleted(destPath);
    return true;
}

/*
 * @brief Exports a conversation to a JSON file.
 * @param convId              UUID of the conversation.
 * @param destPath            Absolute destination file path.
 * @param includeAttachments  Whether to embed base64 attachment data.
 * @return true if successfully written.
 * @sideeffects Writes file to disk. Emits exportCompleted or exportFailed.
 */
bool ExportService::exportToJson(const QString& convId,
                                 const QString& destPath,
                                 bool includeAttachments) {
    VERZETA_ASSERT_MAIN_THREAD();
    const auto convOpt = m_convSvc.getConversation(convId);
    if (!convOpt.has_value()) {
        const QString err = QStringLiteral("Conversation not found: ") + convId;
        qCWarning(verzetaUi) << "ExportService:" << err;
        emit exportFailed(err);
        return false;
    }
    const Conversation& conv = *convOpt;

    const QList<Message> messages = m_msgSvc.getMessages(convId);

    // Build statistics
    int totalTokens = 0;
    double totalCostUsd = 0.0;
    for (const Message& m : messages) {
        totalTokens += m.tokenCount;
        if (m.metadata.contains(QStringLiteral("cost_usd"))) {
            totalCostUsd += m.metadata.value(QStringLiteral("cost_usd")).toDouble();
        }
    }

    // Conversation object
    QJsonObject convObj;
    convObj[QStringLiteral("id")] = conv.id;
    convObj[QStringLiteral("title")] = conv.title;
    convObj[QStringLiteral("created_at")] = conv.createdAt.toString(Qt::ISODate);
    convObj[QStringLiteral("system_prompt")] = conv.systemPrompt;
    convObj[QStringLiteral("llm_config")] = conv.llmConfig;

    // Messages array
    QJsonArray msgsArray;
    for (const Message& msg : messages) {
        msgsArray.append(formatMessageJson(msg, includeAttachments));
    }

    // Statistics object
    QJsonObject statsObj;
    statsObj[QStringLiteral("total_tokens")] = totalTokens;
    statsObj[QStringLiteral("total_messages")] = messages.size();
    statsObj[QStringLiteral("total_cost_usd")] = totalCostUsd;

    // Root document
    QJsonObject root;
    root[QStringLiteral("version")] = QStringLiteral("1.0");
    root[QStringLiteral("conversation")] = convObj;
    root[QStringLiteral("messages")] = msgsArray;
    root[QStringLiteral("statistics")] = statsObj;

    const QJsonDocument doc(root);
    const QString json = doc.toJson(QJsonDocument::Indented);

    if (!writeFile(destPath, json)) {
        const QString err = QStringLiteral("Failed to write file: ") + destPath;
        emit exportFailed(err);
        return false;
    }

    qCInfo(verzetaUi) << "ExportService: JSON export complete:" << destPath;
    emit exportCompleted(destPath);
    return true;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

/**
 * @brief Formats a single message as a Markdown block.
 *
 * The output is literal Markdown. In the samples below, `\n` stands for a
 * newline character. HH:MM:SS is the local creation time, or a single dash
 * character when the message has no valid timestamp, and the model name
 * appears only when the message records one.
 *
 * @verbatim
 *   user:      **You** *(HH:MM:SS)*\n\n{content}\n\n
 *   assistant: **Assistant** *(HH:MM:SS)* · {model}\n\n{content}\n\n
 *   system:    **System** *(HH:MM:SS)*\n\n{content}\n\n
 *   tool:      *(Tool Result)*\n\n{content}\n\n
 * @endverbatim
 *
 * Each tool call recorded on the message is then appended, followed by its
 * result when one was recorded:
 *
 * @verbatim
 *   ```tool_call: {name}({args})```\n
 *   *Result:* {result}\n\n
 * @endverbatim
 *
 * For an assistant message whose finish reason is user_interrupted,
 * `\n\n*[INTERRUPTED]*` is appended after the content.
 *
 * @param msg                The message to format.
 * @param includeAttachments Whether to embed attachment data.
 * @return Markdown string for the message.
 */
QString ExportService::formatMessageMarkdown(const Message& msg, bool includeAttachments) const {
    VERZETA_ASSERT_MAIN_THREAD();
    QString result;
    QTextStream out(&result);

    const QString timeStr = msg.createdAt.isValid()
                                ? msg.createdAt.toLocalTime().toString(QStringLiteral("hh:mm:ss"))
                                : QStringLiteral("-");

    if (msg.role == QStringLiteral("user")) {
        out << QStringLiteral("**You** *(") << timeStr << QStringLiteral(")*\n\n");
        out << msg.content << QStringLiteral("\n\n");
    } else if (msg.role == QStringLiteral("assistant")) {
        out << QStringLiteral("**Assistant** *(") << timeStr << QStringLiteral(")*");
        if (!msg.modelUsed.isEmpty()) {
            out << QStringLiteral(" · ") << msg.modelUsed;
        }
        out << QStringLiteral("\n\n");
        out << msg.content;
        if (msg.finishReason == QStringLiteral("user_interrupted")) {
            out << QStringLiteral("\n\n*[INTERRUPTED]*");
        }
        out << QStringLiteral("\n\n");
    } else if (msg.role == QStringLiteral("system")) {
        out << QStringLiteral("**System** *(") << timeStr << QStringLiteral(")*\n\n");
        out << msg.content << QStringLiteral("\n\n");
    } else if (msg.role == QStringLiteral("tool")) {
        out << QStringLiteral("*(Tool Result)*\n\n");
        out << msg.content << QStringLiteral("\n\n");
    }

    // Append tool calls
    const QList<ToolCall> toolCalls = m_msgSvc.getToolCalls(msg.id);
    for (const ToolCall& tc : toolCalls) {
        const QJsonDocument argsDoc(tc.arguments);
        out << QStringLiteral("```tool_call: ") << tc.toolName << QStringLiteral("(")
            << QString::fromUtf8(argsDoc.toJson(QJsonDocument::Compact))
            << QStringLiteral(")```\n");
        if (!tc.result.isNull() && !tc.result.isUndefined()) {
            const QJsonDocument resDoc(tc.result.isObject() ? QJsonDocument(tc.result.toObject())
                                                            : QJsonDocument(QJsonArray{tc.result}));
            out << QStringLiteral("*Result:* ")
                << QString::fromUtf8(resDoc.toJson(QJsonDocument::Compact))
                << QStringLiteral("\n\n");
        }
    }

    // Append attachment references
    if (includeAttachments) {
        const QList<Attachment> attachments = m_msgSvc.getAttachments(msg.id);
        for (const Attachment& att : attachments) {
            out << QStringLiteral("*Attachment: ") << att.filename << QStringLiteral("*\n");
            if (!att.dataInline.isEmpty()) {
                // Write inline data as base64
                out << QStringLiteral("```base64\n")
                    << QString::fromLatin1(att.dataInline.toBase64())
                    << QStringLiteral("\n```\n\n");
            } else if (!att.dataPath.isEmpty()) {
                // Read on-disk data and embed as base64
                QFile f(att.dataPath);
                if (f.open(QIODevice::ReadOnly)) {
                    const QByteArray data = f.readAll().toBase64();
                    out << QStringLiteral("```base64\n") << QString::fromLatin1(data)
                        << QStringLiteral("\n```\n\n");
                }
            }
        }
    } else {
        const QList<Attachment> attachments = m_msgSvc.getAttachments(msg.id);
        for (const Attachment& att : attachments) {
            out << QStringLiteral("*Attachment: ") << att.filename << QStringLiteral("*\n\n");
        }
    }

    return result;
}

/**
 * @brief Formats a single message as a QJsonObject for JSON export.
 * @param msg                The message to format.
 * @param includeAttachments Whether to embed attachment base64 data.
 * @return QJsonObject with all message fields.
 */
QJsonObject ExportService::formatMessageJson(const Message& msg, bool includeAttachments) const {
    VERZETA_ASSERT_MAIN_THREAD();
    QJsonObject obj;
    obj[QStringLiteral("id")] = msg.id;
    obj[QStringLiteral("role")] = msg.role;
    obj[QStringLiteral("content")] = msg.content;
    obj[QStringLiteral("created_at")] = msg.createdAt.toString(Qt::ISODate);
    obj[QStringLiteral("token_count")] = msg.tokenCount;
    obj[QStringLiteral("model_used")] = msg.modelUsed;
    obj[QStringLiteral("finish_reason")] = msg.finishReason;
    obj[QStringLiteral("metadata")] = msg.metadata;

    // Tool calls
    QJsonArray toolCallsArray;
    const QList<ToolCall> toolCalls = m_msgSvc.getToolCalls(msg.id);
    for (const ToolCall& tc : toolCalls) {
        QJsonObject tcObj;
        tcObj[QStringLiteral("id")] = tc.id;
        tcObj[QStringLiteral("tool_name")] = tc.toolName;
        tcObj[QStringLiteral("arguments")] = tc.arguments;
        tcObj[QStringLiteral("result")] = tc.result;
        tcObj[QStringLiteral("status")] = tc.status;
        tcObj[QStringLiteral("started_at")] = tc.startedAt.toString(Qt::ISODate);
        tcObj[QStringLiteral("completed_at")] = tc.completedAt.toString(Qt::ISODate);
        toolCallsArray.append(tcObj);
    }
    obj[QStringLiteral("tool_calls")] = toolCallsArray;

    // Attachments
    QJsonArray attachmentsArray;
    const QList<Attachment> attachments = m_msgSvc.getAttachments(msg.id);
    for (const Attachment& att : attachments) {
        QJsonObject attObj;
        attObj[QStringLiteral("id")] = att.id;
        attObj[QStringLiteral("file_name")] = att.filename;
        attObj[QStringLiteral("mime_type")] = att.mimeType;
        attObj[QStringLiteral("type")] = att.type;

        if (includeAttachments) {
            if (!att.dataInline.isEmpty()) {
                attObj[QStringLiteral("data_base64")] =
                    QString::fromLatin1(att.dataInline.toBase64());
            } else if (!att.dataPath.isEmpty()) {
                QFile f(att.dataPath);
                if (f.open(QIODevice::ReadOnly)) {
                    attObj[QStringLiteral("data_base64")] =
                        QString::fromLatin1(f.readAll().toBase64());
                }
            }
        }
        attachmentsArray.append(attObj);
    }
    obj[QStringLiteral("attachments")] = attachmentsArray;

    return obj;
}

/**
 * @brief Writes UTF-8 text content to a file at destPath.
 *        Creates parent directories if they do not exist.
 * @param destPath  Absolute file path.
 * @param content   Text to write (UTF-8).
 * @return true if written successfully.
 * @sideeffects Creates or overwrites the file; creates parent directories.
 */
bool ExportService::writeFile(const QString& destPath, const QString& content) const {
    VERZETA_ASSERT_MAIN_THREAD();
    // Ensure parent directory exists
    const QDir dir = QFileInfo(destPath).absoluteDir();
    if (!dir.exists()) {
        if (!QDir().mkpath(dir.absolutePath())) {
            qCWarning(verzetaUi) << "ExportService: failed to create directory:"
                                 << dir.absolutePath();
            return false;
        }
    }

    QFile file(destPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qCWarning(verzetaUi) << "ExportService: failed to open file for writing:" << destPath
                             << file.errorString();
        return false;
    }

    QTextStream stream(&file);
    stream.setEncoding(QStringConverter::Utf8);
    stream << content;
    file.close();

    return true;
}
