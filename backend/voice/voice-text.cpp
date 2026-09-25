// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file voice-text.cpp
 * @brief Markdown-to-speech text normalization and sentence splitting.
 * @layer Utility (Voice)
 * @dependencies Qt6::Core.
 */

#include "voice-text.h"

#include <QRegularExpression>

namespace Verzeta::Voice {

namespace {

/** Shortest fragment kept as its own synthesis chunk; anything shorter
 *  is merged forward so "e.g." does not become an utterance. */
constexpr int kMinChunkChars = 24;

/** Spoken stand-in for a code block. */
const char kCodeCue[] = "Code omitted.";

/**
 * @brief Collapses INDENTED code blocks to the code cue without eating
 *        indented prose.
 *
 * The naive "four spaces means code" rule swallowed nested bullet
 * lists and indented tables, which models emit constantly, and the
 * user heard "Code omitted" in the middle of ordinary prose. Real
 * markdown only treats an indented run as code when it starts after a
 * blank line, and list items shift the indent threshold, so the scan
 * requires BOTH: the run begins after a blank line (or the start of
 * the text), and its first line is not a bullet, a numbered item or a
 * table row. Everything else keeps its lines for the later rules.
 *
 * @param text The message after fenced blocks were collapsed.
 * @returns The text with true indented code replaced by the cue.
 */
QString collapseIndentedCode(const QString& text) {
    static const QRegularExpression indent(QStringLiteral("^(?: {4}|\\t)"));
    static const QRegularExpression listOrTable(
        QStringLiteral("^\\s*(?:[-*+]\\s|\\d+[.)]\\s|\\|)"));
    const QStringList lines = text.split(QLatin1Char('\n'));
    QStringList out;
    out.reserve(lines.size());
    bool prevBlank = true;  // The start of the text counts as blank.
    for (int i = 0; i < lines.size(); ++i) {
        const QString& line = lines.at(i);
        const bool blank = line.trimmed().isEmpty();
        if (prevBlank && !blank && indent.match(line).hasMatch() &&
            !listOrTable.match(line).hasMatch()) {
            // A real indented code block: consume its run (indented
            // lines, plus blank lines followed by more indented code).
            out.append(QLatin1String(kCodeCue));
            while (i + 1 < lines.size()) {
                const QString& next = lines.at(i + 1);
                if (indent.match(next).hasMatch()) {
                    ++i;
                    continue;
                }
                if (next.trimmed().isEmpty() && i + 2 < lines.size() &&
                    indent.match(lines.at(i + 2)).hasMatch()) {
                    i += 2;
                    continue;
                }
                break;
            }
            prevBlank = false;
            continue;
        }
        out.append(line);
        prevBlank = blank;
    }
    return out.join(QLatin1Char('\n'));
}

/**
 * @brief Builds the spoken one-liner for a table: what it is about and
 *        how big it is, instead of reading cells aloud.
 * @param block The matched table text (pipe rows, with or without
 *              outer pipes).
 * @returns E.g. "A table of Feature, Verzeta and Others with 2 rows
 *          is on screen."; degrades to a plain "A table ..." when the
 *          header does not parse.
 */
QString pipeTableSummary(const QString& block) {
    static const QRegularExpression separatorRow(QStringLiteral("^[\\s|:-]+$"));
    const QStringList lines = block.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QStringList header;
    int dataRows = 0;
    for (int i = 0; i < lines.size(); ++i) {
        const QString trimmed = lines.at(i).trimmed();
        if (separatorRow.match(trimmed).hasMatch())
            continue;
        if (header.isEmpty()) {
            // First real row is the header; its cells name the table.
            QString inner = trimmed;
            if (inner.startsWith(QLatin1Char('|')))
                inner.remove(0, 1);
            if (inner.endsWith(QLatin1Char('|')))
                inner.chop(1);
            const QStringList cells = inner.split(QLatin1Char('|'));
            for (const QString& cell : cells) {
                const QString name = cell.trimmed();
                // A header only reads well when it is short prose.
                if (name.isEmpty() || name.size() > 40) {
                    header.clear();
                    break;
                }
                header.append(name);
            }
            if (header.size() < 2)
                header.clear();
            continue;
        }
        ++dataRows;
    }
    QString subject;
    if (!header.isEmpty()) {
        QStringList head = header;
        const QString last = head.takeLast();
        subject = QStringLiteral(" of %1 and %2").arg(head.join(QStringLiteral(", ")), last);
    }
    const QString size =
        dataRows == 1 ? QStringLiteral(" with one row")
                      : (dataRows > 1 ? QStringLiteral(" with %1 rows").arg(dataRows) : QString());
    return QStringLiteral("A table%1%2 is on screen.").arg(subject, size);
}

/**
 * @brief Replaces every match of @p re in @p text with a per-match
 *        table summary (QString::replace can only insert a constant).
 * @param text The text to rewrite in place.
 * @param re   A table-matching expression.
 */
void summarizeTables(QString& text, const QRegularExpression& re) {
    QString out;
    qsizetype last = 0;
    auto it = re.globalMatch(text);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out += QStringView(text).mid(last, m.capturedStart() - last);
        out += pipeTableSummary(m.captured(0));
        last = m.capturedEnd();
    }
    if (last == 0)
        return;  // No match: keep the original allocation.
    out += QStringView(text).mid(last);
    text = out;
}

}  // namespace

QString toSpeakableText(const QString& markdown) {
    if (markdown.trimmed().isEmpty()) {
        return {};
    }
    QString text = markdown;

    // Code first: fenced blocks, then indented blocks, then inline spans.
    // Fenced blocks collapse to one cue however long they are. Indented
    // detection is list- and table-aware (see collapseIndentedCode), so
    // nested bullets and indented tables are NOT eaten as code.
    static const QRegularExpression fenced(QStringLiteral("```[\\s\\S]*?```|~~~[\\s\\S]*?~~~"));
    text.replace(fenced, QLatin1String(kCodeCue));

    text = collapseIndentedCode(text);

    static const QRegularExpression inlineCode(QStringLiteral("`([^`]*)`"));
    text.replace(inlineCode, QStringLiteral("\\1"));

    // Tables, after code so a table inside a fence is already a code cue.
    // Read aloud a table is unlistenable: the pipes and dash rows survive
    // every other rule here and arrive as one run-on line of punctuation.
    // It stays fully visible in the chat; speech gives a one-line
    // summary (what the table is about, how many rows) instead.
    //
    // Two shapes, because both appear in model output. Pipe-led rows are
    // the common one; the header/separator form omits the outer pipes and
    // is recognised by its dash row. Both need at least two consecutive
    // lines, so an ordinary sentence containing a pipe is left alone.
    static const QRegularExpression pipeTable(
        QStringLiteral("(?m)^[ \\t]*\\|.*(?:\\r?\\n[ \\t]*\\|.*)+"));
    summarizeTables(text, pipeTable);

    static const QRegularExpression borderlessTable(
        QStringLiteral("(?m)^[^\\n]*\\|[^\\n]*\\r?\\n[ \\t]*[-:|][-:| \\t]*-{2,}[-:| \\t]*"
                       "(?:\\r?\\n[^\\n]*\\|[^\\n]*)*"));
    summarizeTables(text, borderlessTable);

    // HTML: a table becomes the same summary shape; any other tag is
    // dropped and its text kept, so markup never reaches the voice as
    // angle brackets.
    static const QRegularExpression htmlTable(QStringLiteral("(?is)<table\\b.*?</table\\s*>"));
    if (text.contains(htmlTable)) {
        QString out;
        qsizetype last = 0;
        auto it = htmlTable.globalMatch(text);
        static const QRegularExpression htmlRow(QStringLiteral("(?i)<tr\\b"));
        while (it.hasNext()) {
            const QRegularExpressionMatch m = it.next();
            out += QStringView(text).mid(last, m.capturedStart() - last);
            const int rows = static_cast<int>(m.captured(0).count(htmlRow));
            out += rows > 1 ? QStringLiteral("A table with %1 rows is on "
                                             "screen.")
                                  .arg(rows)
                            : QStringLiteral("A table is on screen.");
            last = m.capturedEnd();
        }
        out += QStringView(text).mid(last);
        text = out;
    }
    static const QRegularExpression htmlTag(QStringLiteral("</?[A-Za-z][^>]*>"));
    text.replace(htmlTag, QStringLiteral(" "));

    // Links and images: keep the label, drop the target.
    static const QRegularExpression image(QStringLiteral("!\\[([^\\]]*)\\]\\([^)]*\\)"));
    text.replace(image, QStringLiteral("\\1"));
    static const QRegularExpression link(QStringLiteral("\\[([^\\]]*)\\]\\([^)]*\\)"));
    text.replace(link, QStringLiteral("\\1"));

    // Block furniture: headings, quotes, list bullets, horizontal rules.
    static const QRegularExpression heading(QStringLiteral("(?m)^\\s*#{1,6}\\s*"));
    text.replace(heading, QString());
    static const QRegularExpression quote(QStringLiteral("(?m)^\\s*>\\s?"));
    text.replace(quote, QString());
    static const QRegularExpression rule(QStringLiteral("(?m)^\\s*(?:[-*_]\\s*){3,}$"));
    text.replace(rule, QString());
    static const QRegularExpression bullet(QStringLiteral("(?m)^\\s*(?:[-*+]|\\d+\\.)\\s+"));
    text.replace(bullet, QString());

    // Emphasis markers around words.
    static const QRegularExpression emphasis(
        QStringLiteral("(\\*{1,3}|_{1,3}|~~)(?=\\S)(.*?)(?<=\\S)\\1"));
    text.replace(emphasis, QStringLiteral("\\2"));

    // "@Alias" is a name when spoken, not an address token.
    static const QRegularExpression mention(QStringLiteral("@([A-Za-z][\\w-]*)"));
    text.replace(mention, QStringLiteral("\\1"));

    // Emoji and other pictographic symbols have no pronunciation.
    QString stripped;
    stripped.reserve(text.size());
    for (const QChar& c : std::as_const(text)) {
        const auto cat = c.category();
        if (cat == QChar::Symbol_Other || cat == QChar::Other_Surrogate) {
            continue;
        }
        stripped.append(c);
    }
    text = stripped;

    // Collapse whitespace so paragraph breaks do not become long pauses.
    static const QRegularExpression spaces(QStringLiteral("\\s+"));
    text.replace(spaces, QStringLiteral(" "));
    text = text.trimmed();

    // A message that was nothing but code says only the cue; if even that
    // is all that remains of an empty body, say nothing.
    return text == QLatin1String(kCodeCue) && markdown.trimmed().isEmpty() ? QString() : text;
}

QStringList splitIntoSentences(const QString& text) {
    const QString clean = text.trimmed();
    if (clean.isEmpty()) {
        return {};
    }
    QStringList chunks;
    int start = 0;
    for (int i = 0; i < clean.size(); ++i) {
        const QChar c = clean.at(i);
        const bool terminator =
            (c == QLatin1Char('.') || c == QLatin1Char('!') || c == QLatin1Char('?'));
        if (!terminator) {
            continue;
        }
        // A sentence ends at punctuation followed by whitespace (or the
        // end of the text), never mid-token like "3.5" or "e.g.".
        const bool atEnd = (i + 1 >= clean.size());
        if (!atEnd && !clean.at(i + 1).isSpace()) {
            continue;
        }
        const QString candidate = clean.mid(start, i - start + 1).trimmed();
        if (candidate.size() < kMinChunkChars && !atEnd) {
            continue;  // Too short to stand alone; merge into the next.
        }
        if (!candidate.isEmpty()) {
            chunks.append(candidate);
        }
        start = i + 1;
    }
    const QString tail = clean.mid(start).trimmed();
    if (!tail.isEmpty()) {
        if (tail.size() < kMinChunkChars && !chunks.isEmpty()) {
            chunks.last().append(QLatin1Char(' ') + tail);
        } else {
            chunks.append(tail);
        }
    }
    return chunks;
}

}  // namespace Verzeta::Voice
