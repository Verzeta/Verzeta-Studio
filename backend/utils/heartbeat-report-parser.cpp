// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file heartbeat-report-parser.cpp
 * @brief Implementation of the subagent structured-output parser.
 *        Splits "TITLE:" / "RESULTS:" / "SUMMARY:" sections in a
 *        tolerant way.  Never throws.
 *
 *        Input model: the labels appear ONCE each, in the order
 *        TITLE → RESULTS → SUMMARY.  The first label matches case-
 *        insensitively at the start of a line; everything between
 *        consecutive label lines is the previous label's content.
 *        Trailing whitespace is trimmed from each section.  The
 *        TITLE section is collapsed to a single line and truncated
 *        to 100 chars (matches the prompt-addendum constraint
 *        imposed by RequestBuilder::buildSubagentRequest).
 *
 * @layer Utility (pure function)
 * @dependencies Qt6::Core (QStringList).  No service deps.
 */


#include "heartbeat-report-parser.h"

#include <QStringList>

namespace {

/**
 * @brief Returns the section name found at the start of a line, or
 *        empty string if the line is not a section label.
 *        Recognises (case-insensitive): "TITLE:", "RESULTS:",
 *        "SUMMARY:". A label MUST start the line (after stripping any
 *        leading horizontal whitespace) and be followed by ':'.
 */
QString labelOnLine(const QString& line) {
    // Strip leading horizontal whitespace only — don't trim trailing,
    // because content can follow the label on the same line.
    int i = 0;
    while (i < line.size() && (line[i] == QLatin1Char(' ') || line[i] == QLatin1Char('\t'))) {
        ++i;
    }
    const QString rest = line.mid(i);

    // Anchored, case-insensitive, label-then-colon match.
    auto matches = [&](const char* tag) {
        const int n = static_cast<int>(QString(QLatin1String(tag)).size());
        if (rest.size() < n + 1)
            return false;
        if (rest[n] != QLatin1Char(':'))
            return false;
        return rest.left(n).compare(QString::fromLatin1(tag), Qt::CaseInsensitive) == 0;
    };
    if (matches("TITLE"))
        return QStringLiteral("TITLE");
    if (matches("RESULTS"))
        return QStringLiteral("RESULTS");
    if (matches("SUMMARY"))
        return QStringLiteral("SUMMARY");
    return {};
}

/**
 * @brief Returns content after the first ':' on the line (after the label),
 *        or empty if no content follows on that same line.
 */
QString contentAfterLabel(const QString& line) {
    const int colon = line.indexOf(QLatin1Char(':'));
    if (colon < 0)
        return {};
    return line.mid(colon + 1);
}

}  // anonymous namespace

ParsedHeartbeatReport parseHeartbeatReport(const QString& raw) {
    ParsedHeartbeatReport out;
    if (raw.isEmpty()) {
        return out;
    }

    // Split on '\n' but keep '\r' tolerance — strip a trailing '\r' on
    // each line so Windows-style CRLF inputs parse the same as LF.
    QStringList lines = raw.split(QLatin1Char('\n'));
    for (auto& s : lines) {
        if (s.endsWith(QLatin1Char('\r')))
            s.chop(1);
    }

    QString currentSection;  // "TITLE" | "RESULTS" | "SUMMARY"
    QStringList titleAcc, bodyAcc, summaryAcc;

    auto flush = [&](const QString& section, const QString& content) {
        if (section == QStringLiteral("TITLE")) {
            titleAcc << content;
        } else if (section == QStringLiteral("RESULTS")) {
            bodyAcc << content;
        } else if (section == QStringLiteral("SUMMARY")) {
            summaryAcc << content;
        }
    };

    for (const QString& line : lines) {
        const QString lab = labelOnLine(line);
        if (!lab.isEmpty()) {
            // Section change. Capture any content trailing the label
            // on this same line as the FIRST content of the new section.
            currentSection = lab;
            const QString sameLine = contentAfterLabel(line).trimmed();
            if (!sameLine.isEmpty()) {
                flush(currentSection, sameLine);
            }
            continue;
        }
        if (!currentSection.isEmpty()) {
            flush(currentSection, line);
        }
        // If we haven't seen a label yet, drop the line — it's
        // pre-amble. The model is instructed to emit ONLY the three
        // labelled sections. If the model includes a pre-amble we
        // tolerate it by ignoring those lines.
    }

    // Build outputs.
    out.title = titleAcc.join(QLatin1Char(' ')).trimmed();
    if (out.title.size() > 100) {
        out.title = out.title.left(100);
    }
    out.body = bodyAcc.join(QLatin1Char('\n')).trimmed();
    out.summary = summaryAcc.join(QLatin1Char('\n')).trimmed();

    // valid iff we found at least RESULTS or SUMMARY. (TITLE alone is
    // not enough — the model produced no actual content.)
    out.valid = !out.body.isEmpty() || !out.summary.isEmpty();

    if (!out.valid) {
        // Diagnostic-preservation fallback: keep the raw text in `body`
        // so heartbeat_reports records what the model said even when
        // labels were missing.
        out.body = raw.trimmed();
    }
    return out;
}
