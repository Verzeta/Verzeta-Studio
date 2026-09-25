// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file markdown-utils.h
 * @brief Converts Markdown text to sanitized HTML for rich-text display
 *        in QML. Uses Qt's built-in QTextDocument Markdown support.
 *        Provides code-block extraction and content segmentation for the
 *        interleaved MarkdownText + CodeBlock rendering strategy.
 * @layer Utility
 * @dependencies Qt6::Gui (QTextDocument)
 */

#pragma once

#include <QObject>
#include <QString>
#include <QVariantList>

/**
 * @brief Stateless Markdown-to-HTML converter exposed as a QML singleton.
 *
 * All methods are Q_INVOKABLE so they are callable from QML:
 *   MarkdownConverter.toHtml(rawText)
 *   MarkdownConverter.splitContentSegments(rawText)
 *   MarkdownConverter.extractCodeBlocks(rawText)
 *
 * Security: sanitizeHtml() strips \<script\>, \<iframe\>, \<object\>, \<embed\>
 * tags and all on* event handler attributes to prevent XSS from
 * LLM-generated content rendered in QML Text elements.
 *
 * Registration: Registered as QML singleton "MarkdownConverter" in
 * org.verzeta.studio via AppController::registerTypes().
 *
 * Thread safety: All methods are stateless and re-entrant (no mutable state).
 */
class MarkdownConverter : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Returns the process-wide singleton instance.
     * @return Reference to the singleton MarkdownConverter.
     */
    static MarkdownConverter& instance();

    /**
     * @brief Converts Markdown text to sanitized HTML.
     * @param markdown Raw Markdown text (may contain code fences, bold, links, etc.).
     * @return HTML string safe for display in a QML Text element with
     *         textFormat: Text.RichText. All XSS vectors are stripped.
     * @complexity O(n) where n is the length of the input.
     * @sideeffects None. Pure function.
     *
     * Process:
     *   1. QTextDocument::setMarkdown(): Qt native conversion.
     *   2. sanitizeHtml(): strip dangerous tags and attributes.
     *   3. Return the sanitized HTML string.
     */
    Q_INVOKABLE QString toHtml(const QString& markdown);

    /**
     * @brief Extracts all code blocks from Markdown as a JSON array string.
     * @param markdown Raw Markdown text.
     * @return JSON array: [{"language":"python","code":"print('hi')","lineStart":5}, ...]
     *         Returns "[]" if no code blocks are found.
     * @complexity O(n): single regex pass.
     * @sideeffects None.
     */
    Q_INVOKABLE QString extractCodeBlocks(const QString& markdown);

    /**
     * @brief Splits Markdown into interleaved text and code segments.
     * @param markdown Raw Markdown text.
     * @return QVariantList of QVariantMap objects, each with:
     *           { "type": "text"|"code", "content": \<string\>, "language": \<string\> }
     *         Text segments have an empty "language" field.
     * @complexity O(n): single regex pass.
     * @sideeffects None.
     *
     * Used by MessageBubble.qml to render text via MarkdownText and code via
     * CodeBlock, interleaved in a ColumnLayout.
     */
    Q_INVOKABLE QVariantList splitContentSegments(const QString& markdown);

  private:
    /**
     * @brief Private constructor; use instance() to obtain the singleton.
     */
    MarkdownConverter() = default;

    /**
     * @brief Strips dangerous HTML tags and attributes to prevent XSS.
     * @param html Raw HTML from QTextDocument::toHtml().
     * @return Sanitized HTML with the following removed:
     *           - \<script\>...\</script\> and contents
     *           - \<iframe\>...\</iframe\> and contents
     *           - \<object\>...\</object\> and contents
     *           - \<embed\> tags
     *           - on* event handler attributes (onerror, onclick, etc.)
     *           - javascript: href values
     * @complexity O(n): multiple regex passes over the HTML string.
     * @sideeffects None.
     */
    QString sanitizeHtml(const QString& html);
};
