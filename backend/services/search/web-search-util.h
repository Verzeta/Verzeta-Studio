// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file web-search-util.h
 * @brief Small pure helpers shared by web-search providers: HTML→text
 *        stripping with entity decoding, and DuckDuckGo redirect-URL
 *        unwrapping. Header-only inline so providers and tests share one
 *        implementation (no duplication, no extra translation unit).
 * @layer Service (Search subsystem)
 * @dependencies Qt6::Core (QString, QRegularExpression, QUrl).
 */
#pragma once

#include <QRegularExpression>
#include <QString>
#include <QStringView>
#include <QUrl>
#include <QUrlQuery>

namespace Search {

/**
 * @brief Strip HTML tags and decode common entities into plain text.
 * @param html Fragment of HTML (e.g. a result title or snippet).
 * @returns Whitespace-collapsed, trimmed plain text.
 *
 * Deterministic and thread-safe (no QTextDocument): tags are removed by
 * regex, then a fixed set of named entities plus decimal/hex numeric
 * character references are decoded. Intended for short search snippets,
 * not general HTML rendering.
 */
inline QString stripHtmlToText(const QString& html) {
    QString s = html;

    // Remove tags.
    static const QRegularExpression tagRe(QStringLiteral("<[^>]*>"));
    s.remove(tagRe);

    // Decode numeric character references (&#1234; and &#x1F600;).
    static const QRegularExpression numRe(QStringLiteral("&#(x?)([0-9A-Fa-f]+);"));
    QString out;
    out.reserve(s.size());
    int last = 0;
    auto it = numRe.globalMatch(s);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out += QStringView{s}.mid(last, m.capturedStart() - last).toString();
        const bool hex = !m.captured(1).isEmpty();
        bool ok = false;
        const uint code = m.captured(2).toUInt(&ok, hex ? 16 : 10);
        if (ok && code != 0) {
            out += QChar(code);
        }
        last = m.capturedEnd();
    }
    out += QStringView{s}.mid(last).toString();
    s = out;

    // Decode the common named entities (order: &amp; last would double-decode,
    // so decode &amp; FIRST is wrong — decode it LAST to avoid turning
    // "&amp;lt;" into "<". We decode &amp; last deliberately).
    s.replace(QLatin1String("&lt;"), QLatin1String("<"));
    s.replace(QLatin1String("&gt;"), QLatin1String(">"));
    s.replace(QLatin1String("&quot;"), QLatin1String("\""));
    s.replace(QLatin1String("&apos;"), QLatin1String("'"));
    s.replace(QLatin1String("&#39;"), QLatin1String("'"));
    s.replace(QLatin1String("&nbsp;"), QLatin1String(" "));
    s.replace(QLatin1String("&amp;"), QLatin1String("&"));

    // Collapse runs of whitespace.
    static const QRegularExpression wsRe(QStringLiteral("\\s+"));
    s.replace(wsRe, QStringLiteral(" "));
    return s.trimmed();
}

/**
 * @brief Unwrap a DuckDuckGo redirect URL to its real target.
 * @param href A raw href from a DDG result anchor.
 * @returns The decoded target if `href` is a `…/l/?…uddg=<target>…`
 *          redirect; otherwise `href` unchanged.
 *
 * The lite endpoint usually returns direct URLs, but the html endpoint
 * (and some lite rows) wrap them in a `/l/?uddg=` redirector. Handled
 * defensively so both shapes yield a usable absolute URL.
 */
inline QString unwrapDdgRedirect(const QString& href) {
    if (!href.contains(QLatin1String("/l/?")) || !href.contains(QLatin1String("uddg="))) {
        return href;
    }
    // Normalise scheme-relative ("//duckduckgo.com/l/?…") for QUrl parsing.
    QString normalised = href;
    if (normalised.startsWith(QLatin1String("//"))) {
        normalised.prepend(QLatin1String("https:"));
    } else if (normalised.startsWith(QLatin1Char('/'))) {
        normalised.prepend(QLatin1String("https://duckduckgo.com"));
    }
    const QUrl u(normalised);
    const QString target = QUrlQuery(u).queryItemValue(QStringLiteral("uddg"), QUrl::FullyDecoded);
    return target.isEmpty() ? href : target;
}

}  // namespace Search
