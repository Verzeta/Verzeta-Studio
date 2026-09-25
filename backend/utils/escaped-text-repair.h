// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file escaped-text-repair.h
 * @brief Detects and repairs LLM tool-call content whose newlines
 *        arrived double-escaped (literal two-character "\n" sequences in
 *        place of real newlines), so file and canvas writes receive the
 *        text the model intended instead of a single-line escape soup.
 * @layer Utility
 * @dependencies Qt6::Core (QString, QJsonDocument)
 */

#pragma once

#include <QString>

namespace Verzeta {

/**
 * @brief Returns true when @p content carries the double-escaped-newline
 *        signature: no real newline anywhere, at least three literal
 *        backslash-n sequences, and the text is not a valid JSON document.
 *
 *        The JSON exclusion exists because a minified JSON document
 *        legitimately stores newlines as backslash-n INSIDE its string
 *        values on a single physical line, and converting those to raw
 *        control characters would corrupt the document.
 * @param content Candidate tool-call text payload.
 * @returns true when repairDoubleEscapedText() would transform the input.
 */
bool looksDoubleEscaped(const QString& content);

/**
 * @brief Repairs double-escaped whitespace escapes in @p content when,
 *        and only when, looksDoubleEscaped() holds. Literal "\r\n" and
 *        "\n" become real newlines and literal "\t" becomes a real tab.
 *        Quote and backslash escapes are deliberately left untouched:
 *        the observed failure mode double-escapes whitespace escapes
 *        only, and rewriting other escapes could damage legitimate
 *        content.
 * @param content Candidate tool-call text payload.
 * @returns The repaired text, or @p content unchanged when the signature
 *          does not hold.
 */
QString repairDoubleEscapedText(const QString& content);

}  // namespace Verzeta
