// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file escaped-text-repair.cpp
 * @brief Detection + repair of double-escaped newline payloads in
 *        LLM tool-call content (write_file / open_canvas / edit_canvas).
 * @layer Utility
 * @dependencies Qt6::Core (QString, QJsonDocument).
 */


#include "escaped-text-repair.h"

#include <QJsonDocument>
#include <QLatin1String>

namespace Verzeta {

namespace {
// Minimum number of literal backslash-n sequences before a newline-free
// text is treated as double-escaped. One or two occurrences on a single
// line are plausible as genuine content (a shell one-liner quoting
// "\n", a printf format string); a document's worth of them with not a
// single real newline is the corruption signature (live incident:
// 47 and 99 occurrences).
constexpr int kMinLiteralNewlines = 3;
}  // namespace

bool looksDoubleEscaped(const QString& content) {
    if (content.isEmpty())
        return false;
    if (content.contains(QLatin1Char('\n')))
        return false;

    const int literalCount = content.count(QLatin1String("\\n"));
    if (literalCount < kMinLiteralNewlines)
        return false;

    // A minified JSON document legitimately carries \n escapes inside
    // its string values on one physical line; converting them to raw
    // control characters would make the document invalid JSON.
    QJsonParseError err;
    QJsonDocument::fromJson(content.toUtf8(), &err);
    if (err.error == QJsonParseError::NoError)
        return false;

    return true;
}

QString repairDoubleEscapedText(const QString& content) {
    if (!looksDoubleEscaped(content))
        return content;

    QString repaired = content;
    // Order matters: collapse the Windows pair first so "\r\n" does not
    // leave a stray literal "\r" behind after the "\n" replacement.
    repaired.replace(QLatin1String("\\r\\n"), QLatin1String("\n"));
    repaired.replace(QLatin1String("\\n"), QLatin1String("\n"));
    repaired.replace(QLatin1String("\\t"), QLatin1String("\t"));
    return repaired;
}

}  // namespace Verzeta
