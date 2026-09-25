// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file markdown-utils.cpp
 * @brief Implementation of Markdown-to-HTML conversion with XSS sanitization,
 *        code-block extraction, and content segmentation for interleaved rendering.
 * @layer Utility
 * @dependencies Qt6::Gui (QTextDocument), Qt6::Core (QRegularExpression, QJsonArray)
 */


#include "markdown-utils.h"

#include <QTextDocument>

#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>

namespace {

/**
 * @brief Convert common inline LaTeX/TeX into readable Unicode so models'
 *        math markup (e.g. "$\rightarrow$") renders as text instead of raw
 *        source. Applied ONLY to prose passed to toHtml(); fenced code
 *        blocks are separate segments (rendered by the CodeBlock delegate,
 *        never through toHtml) so code is never touched. Inline `code`
 *        spans within prose are a minor known exception.
 *
 * Steps: (1) drop math delimiters keeping their inner content, covering display
 * `$$…$$`, `\[…\]`, `\(…\)`, and inline `$…$` ONLY when the content
 * contains a backslash (so currency like "$5" is left alone); (2) map
 * `\command` tokens to Unicode (unknown commands are left verbatim, so a
 * Windows path like C:\Users is untouched); (3) drop spacing commands.
 * Idempotent: a second pass is a no-op.
 */
QString convertCommonLatex(const QString& in) {
    QString s = in;

    // (1) Strip math delimiters, keep inner content.
    static const QRegularExpression displayMath(QStringLiteral("\\$\\$([\\s\\S]+?)\\$\\$"));
    s.replace(displayMath, QStringLiteral("\\1"));
    static const QRegularExpression bracketMath(QStringLiteral("\\\\\\[([\\s\\S]+?)\\\\\\]"));
    s.replace(bracketMath, QStringLiteral("\\1"));
    static const QRegularExpression parenMath(QStringLiteral("\\\\\\(([\\s\\S]+?)\\\\\\)"));
    s.replace(parenMath, QStringLiteral("\\1"));
    // Inline $...$ only when it contains a backslash (avoids "$5" currency).
    static const QRegularExpression inlineMath(QStringLiteral("\\$([^$\\n]*\\\\[^$\\n]*)\\$"));
    s.replace(inlineMath, QStringLiteral("\\1"));

    // (2) Map \command tokens to Unicode.
    static const QHash<QString, QString> kMap = {
        // arrows
        {QStringLiteral("rightarrow"), QStringLiteral("→")},
        {QStringLiteral("to"), QStringLiteral("→")},
        {QStringLiteral("Rightarrow"), QStringLiteral("⇒")},
        {QStringLiteral("implies"), QStringLiteral("⇒")},
        {QStringLiteral("leftarrow"), QStringLiteral("←")},
        {QStringLiteral("gets"), QStringLiteral("←")},
        {QStringLiteral("Leftarrow"), QStringLiteral("⇐")},
        {QStringLiteral("leftrightarrow"), QStringLiteral("↔")},
        {QStringLiteral("Leftrightarrow"), QStringLiteral("⇔")},
        {QStringLiteral("iff"), QStringLiteral("⇔")},
        {QStringLiteral("uparrow"), QStringLiteral("↑")},
        {QStringLiteral("downarrow"), QStringLiteral("↓")},
        {QStringLiteral("mapsto"), QStringLiteral("↦")},
        // operators
        {QStringLiteral("times"), QStringLiteral("×")},
        {QStringLiteral("div"), QStringLiteral("÷")},
        {QStringLiteral("pm"), QStringLiteral("±")},
        {QStringLiteral("mp"), QStringLiteral("∓")},
        {QStringLiteral("cdot"), QStringLiteral("·")},
        {QStringLiteral("ast"), QStringLiteral("∗")},
        {QStringLiteral("star"), QStringLiteral("⋆")},
        {QStringLiteral("circ"), QStringLiteral("∘")},
        {QStringLiteral("bullet"), QStringLiteral("•")},
        // relations
        {QStringLiteral("leq"), QStringLiteral("≤")},
        {QStringLiteral("le"), QStringLiteral("≤")},
        {QStringLiteral("geq"), QStringLiteral("≥")},
        {QStringLiteral("ge"), QStringLiteral("≥")},
        {QStringLiteral("neq"), QStringLiteral("≠")},
        {QStringLiteral("ne"), QStringLiteral("≠")},
        {QStringLiteral("approx"), QStringLiteral("≈")},
        {QStringLiteral("equiv"), QStringLiteral("≡")},
        {QStringLiteral("sim"), QStringLiteral("∼")},
        {QStringLiteral("simeq"), QStringLiteral("≃")},
        {QStringLiteral("cong"), QStringLiteral("≅")},
        {QStringLiteral("propto"), QStringLiteral("∝")},
        {QStringLiteral("ll"), QStringLiteral("≪")},
        {QStringLiteral("gg"), QStringLiteral("≫")},
        // sets / logic
        {QStringLiteral("in"), QStringLiteral("∈")},
        {QStringLiteral("notin"), QStringLiteral("∉")},
        {QStringLiteral("subset"), QStringLiteral("⊂")},
        {QStringLiteral("subseteq"), QStringLiteral("⊆")},
        {QStringLiteral("supset"), QStringLiteral("⊃")},
        {QStringLiteral("supseteq"), QStringLiteral("⊇")},
        {QStringLiteral("cup"), QStringLiteral("∪")},
        {QStringLiteral("cap"), QStringLiteral("∩")},
        {QStringLiteral("emptyset"), QStringLiteral("∅")},
        {QStringLiteral("setminus"), QStringLiteral("∖")},
        {QStringLiteral("forall"), QStringLiteral("∀")},
        {QStringLiteral("exists"), QStringLiteral("∃")},
        {QStringLiteral("neg"), QStringLiteral("¬")},
        {QStringLiteral("land"), QStringLiteral("∧")},
        {QStringLiteral("lor"), QStringLiteral("∨")},
        {QStringLiteral("wedge"), QStringLiteral("∧")},
        {QStringLiteral("vee"), QStringLiteral("∨")},
        // misc symbols
        {QStringLiteral("infty"), QStringLiteral("∞")},
        {QStringLiteral("partial"), QStringLiteral("∂")},
        {QStringLiteral("nabla"), QStringLiteral("∇")},
        {QStringLiteral("sqrt"), QStringLiteral("√")},
        {QStringLiteral("sum"), QStringLiteral("∑")},
        {QStringLiteral("prod"), QStringLiteral("∏")},
        {QStringLiteral("int"), QStringLiteral("∫")},
        {QStringLiteral("degree"), QStringLiteral("°")},
        {QStringLiteral("deg"), QStringLiteral("°")},
        {QStringLiteral("angle"), QStringLiteral("∠")},
        {QStringLiteral("perp"), QStringLiteral("⊥")},
        {QStringLiteral("parallel"), QStringLiteral("∥")},
        {QStringLiteral("ldots"), QStringLiteral("…")},
        {QStringLiteral("dots"), QStringLiteral("…")},
        {QStringLiteral("cdots"), QStringLiteral("⋯")},
        // greek (lower)
        {QStringLiteral("alpha"), QStringLiteral("α")},
        {QStringLiteral("beta"), QStringLiteral("β")},
        {QStringLiteral("gamma"), QStringLiteral("γ")},
        {QStringLiteral("delta"), QStringLiteral("δ")},
        {QStringLiteral("epsilon"), QStringLiteral("ε")},
        {QStringLiteral("varepsilon"), QStringLiteral("ε")},
        {QStringLiteral("zeta"), QStringLiteral("ζ")},
        {QStringLiteral("eta"), QStringLiteral("η")},
        {QStringLiteral("theta"), QStringLiteral("θ")},
        {QStringLiteral("iota"), QStringLiteral("ι")},
        {QStringLiteral("kappa"), QStringLiteral("κ")},
        {QStringLiteral("lambda"), QStringLiteral("λ")},
        {QStringLiteral("mu"), QStringLiteral("μ")},
        {QStringLiteral("nu"), QStringLiteral("ν")},
        {QStringLiteral("xi"), QStringLiteral("ξ")},
        {QStringLiteral("pi"), QStringLiteral("π")},
        {QStringLiteral("rho"), QStringLiteral("ρ")},
        {QStringLiteral("sigma"), QStringLiteral("σ")},
        {QStringLiteral("tau"), QStringLiteral("τ")},
        {QStringLiteral("upsilon"), QStringLiteral("υ")},
        {QStringLiteral("phi"), QStringLiteral("φ")},
        {QStringLiteral("varphi"), QStringLiteral("φ")},
        {QStringLiteral("chi"), QStringLiteral("χ")},
        {QStringLiteral("psi"), QStringLiteral("ψ")},
        {QStringLiteral("omega"), QStringLiteral("ω")},
        // greek (upper)
        {QStringLiteral("Gamma"), QStringLiteral("Γ")},
        {QStringLiteral("Delta"), QStringLiteral("Δ")},
        {QStringLiteral("Theta"), QStringLiteral("Θ")},
        {QStringLiteral("Lambda"), QStringLiteral("Λ")},
        {QStringLiteral("Xi"), QStringLiteral("Ξ")},
        {QStringLiteral("Pi"), QStringLiteral("Π")},
        {QStringLiteral("Sigma"), QStringLiteral("Σ")},
        {QStringLiteral("Phi"), QStringLiteral("Φ")},
        {QStringLiteral("Psi"), QStringLiteral("Ψ")},
        {QStringLiteral("Omega"), QStringLiteral("Ω")},
        // sizing/grouping — drop (keep any following delimiter)
        {QStringLiteral("left"), QString()},
        {QStringLiteral("right"), QString()},
        {QStringLiteral("quad"), QStringLiteral(" ")},
        {QStringLiteral("qquad"), QStringLiteral("  ")},
    };
    static const QRegularExpression cmdRe(QStringLiteral("\\\\([A-Za-z]+)(?![A-Za-z])"));
    QString out;
    out.reserve(s.size());
    int last = 0;
    auto it = cmdRe.globalMatch(s);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        out += QStringView{s}.mid(last, m.capturedStart() - last).toString();
        const auto f = kMap.constFind(m.captured(1));
        // Unknown command → leave verbatim (e.g. C:\Users, \frac).
        out += (f != kMap.constEnd()) ? f.value() : m.captured(0);
        last = m.capturedEnd();
    }
    out += QStringView{s}.mid(last).toString();
    s = out;

    // (3) Drop thin/medium/neg spacing commands (\, \; \: \!).
    static const QRegularExpression spacing(QStringLiteral("\\\\[,;:!]"));
    s.replace(spacing, QString());

    return s;
}

}  // namespace

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------

/**
 * @brief Returns the process-wide singleton instance.
 * @return Reference to the static MarkdownConverter instance.
 */
MarkdownConverter& MarkdownConverter::instance() {
    static MarkdownConverter inst;
    return inst;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------

/*
 * @brief Converts Markdown to sanitized HTML using Qt's QTextDocument.
 * @param markdown Input Markdown text.
 * @return Sanitized HTML ready for QML Text { textFormat: Text.RichText }.
 * @complexity O(n).
 * @sideeffects None.
 */
QString MarkdownConverter::toHtml(const QString& markdown) {
    if (markdown.isEmpty()) {
        return {};
    }

    // Convert common inline LaTeX (e.g. "$\rightarrow$") to Unicode before
    // the markdown parser sees it, so math markup renders as readable text
    // rather than raw source. Code blocks are separate segments and never
    // reach toHtml, so they are unaffected.
    const QString prepared = convertCommonLatex(markdown);

    QTextDocument doc;
    doc.setMarkdown(prepared, QTextDocument::MarkdownDialectGitHub);
    QString html = doc.toHtml();
    return sanitizeHtml(html);
}

/*
 * @brief Extracts code fences from Markdown and returns them as a JSON array.
 * @param markdown Input Markdown text.
 * @return JSON array string with objects {language, code, lineStart}.
 * @complexity O(n).
 * @sideeffects None.
 */
QString MarkdownConverter::extractCodeBlocks(const QString& markdown) {
    if (markdown.isEmpty()) {
        return QStringLiteral("[]");
    }

    QJsonArray blocks;

    // Match triple-backtick fences: ```lang\ncontent\n```
    static const QRegularExpression fence(QStringLiteral("```([^\\n]*)\\n([\\s\\S]*?)```"),
                                          QRegularExpression::MultilineOption);

    int lineOffset = 0;
    QRegularExpressionMatchIterator it = fence.globalMatch(markdown);
    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();

        // Count newlines up to the match start to compute lineStart
        const int startPos = static_cast<int>(m.capturedStart());
        const QString before = markdown.left(startPos);
        int lineStart = static_cast<int>(before.count(QLatin1Char('\n'))) + 1;

        QJsonObject block;
        block[QStringLiteral("language")] = m.captured(1).trimmed();
        block[QStringLiteral("code")] = m.captured(2);
        block[QStringLiteral("lineStart")] = lineStart;
        blocks.append(block);
    }

    return QJsonDocument(blocks).toJson(QJsonDocument::Compact);
}

/*
 * @brief Splits Markdown into interleaved text and code segments.
 *        Used by MessageBubble.qml to select MarkdownText vs CodeBlock delegates.
 * @param markdown Input Markdown text.
 * @return List of QVariantMaps: {type, content, language}.
 * @complexity O(n).
 * @sideeffects None.
 */
QVariantList MarkdownConverter::splitContentSegments(const QString& markdown) {
    if (markdown.isEmpty()) {
        return {};
    }

    QVariantList segments;

    // Match triple-backtick fences: ```lang\ncontent\n```
    static const QRegularExpression fence(QStringLiteral("```([^\\n]*)\\n([\\s\\S]*?)```"),
                                          QRegularExpression::MultilineOption);

    int lastEnd = 0;
    QRegularExpressionMatchIterator it = fence.globalMatch(markdown);

    while (it.hasNext()) {
        const QRegularExpressionMatch m = it.next();
        const int matchStart = static_cast<int>(m.capturedStart());

        // Text segment before this code fence
        if (matchStart > lastEnd) {
            const QString text = markdown.mid(lastEnd, matchStart - lastEnd).trimmed();
            if (!text.isEmpty()) {
                QVariantMap seg;
                seg[QStringLiteral("type")] = QStringLiteral("text");
                seg[QStringLiteral("content")] = text;
                seg[QStringLiteral("language")] = QString{};
                segments.append(seg);
            }
        }

        // Code segment
        QVariantMap seg;
        seg[QStringLiteral("type")] = QStringLiteral("code");
        seg[QStringLiteral("content")] = m.captured(2);
        seg[QStringLiteral("language")] = m.captured(1).trimmed();
        segments.append(seg);

        lastEnd = static_cast<int>(m.capturedEnd());
    }

    // Trailing text after the last CLOSED code fence. This may itself contain an
    // UNTERMINATED opening fence — an agent that emits "```markdown\n# ...\n" and
    // never writes the closing "```". Such a fence must be split off as a code
    // segment too: otherwise the whole tail reaches QTextDocument::setMarkdown,
    // whose parser treats an unterminated fence as a code block running to the end
    // of the document — so the entire remainder renders as flat preformatted text
    // ("markdown not rendering"). Emitting it as a code segment instead shows it in
    // a distinct code block (raw content) and stops it poisoning the prose render.
    if (lastEnd < markdown.length()) {
        const QString rest = markdown.mid(lastEnd);
        // An opening fence with no matching close: ```lang\n then everything to EOF.
        static const QRegularExpression openFence(QStringLiteral("```([^\\n]*)\\n([\\s\\S]*)$"));
        const QRegularExpressionMatch om = openFence.match(rest);
        if (om.hasMatch()) {
            const QString before = rest.left(static_cast<int>(om.capturedStart())).trimmed();
            if (!before.isEmpty()) {
                QVariantMap seg;
                seg[QStringLiteral("type")] = QStringLiteral("text");
                seg[QStringLiteral("content")] = before;
                seg[QStringLiteral("language")] = QString{};
                segments.append(seg);
            }
            QVariantMap codeSeg;
            codeSeg[QStringLiteral("type")] = QStringLiteral("code");
            codeSeg[QStringLiteral("content")] = om.captured(2);
            codeSeg[QStringLiteral("language")] = om.captured(1).trimmed();
            segments.append(codeSeg);
        } else {
            const QString text = rest.trimmed();
            if (!text.isEmpty()) {
                QVariantMap seg;
                seg[QStringLiteral("type")] = QStringLiteral("text");
                seg[QStringLiteral("content")] = text;
                seg[QStringLiteral("language")] = QString{};
                segments.append(seg);
            }
        }
    }

    // If no code fences were found, return the whole content as a single text segment
    if (segments.isEmpty()) {
        QVariantMap seg;
        seg[QStringLiteral("type")] = QStringLiteral("text");
        seg[QStringLiteral("content")] = markdown;
        seg[QStringLiteral("language")] = QString{};
        segments.append(seg);
    }

    return segments;
}

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

/**
 * @brief Strips XSS vectors from HTML produced by QTextDocument.
 * @param html Raw HTML string.
 * @return Sanitized HTML with dangerous elements and attributes removed.
 * @complexity O(n): 5 sequential regex passes.
 * @sideeffects None.
 *
 * Strips:
 *   \<script\>...\</script\>   JS execution
 *   \<iframe\>...\</iframe\>   embedded frames
 *   \<object\>...\</object\>   plugin embeds
 *   <embed ...>            plugin embeds
 *   on*="..."              event handler attributes
 *   javascript: hrefs      JS URL execution
 */
QString MarkdownConverter::sanitizeHtml(const QString& html) {
    QString result = html;

    // 1. Remove <script> blocks (including content)
    static const QRegularExpression scriptTag(QStringLiteral("<script[^>]*>[\\s\\S]*?</script>"),
                                              QRegularExpression::CaseInsensitiveOption);
    result.remove(scriptTag);

    // 2. Remove <iframe> blocks
    static const QRegularExpression iframeTag(QStringLiteral("<iframe[^>]*>[\\s\\S]*?</iframe>"),
                                              QRegularExpression::CaseInsensitiveOption);
    result.remove(iframeTag);

    // 3. Remove <object> blocks
    static const QRegularExpression objectTag(QStringLiteral("<object[^>]*>[\\s\\S]*?</object>"),
                                              QRegularExpression::CaseInsensitiveOption);
    result.remove(objectTag);

    // 4. Remove <embed> tags (void element — no closing tag)
    static const QRegularExpression embedTag(QStringLiteral("<embed[^>]*/?>"),
                                             QRegularExpression::CaseInsensitiveOption);
    result.remove(embedTag);

    // 5. Remove on* event handler attributes (e.g., onerror="...", onclick='...')
    static const QRegularExpression onEventAttr(
        QStringLiteral("\\s+on[a-zA-Z]+\\s*=\\s*(?:\"[^\"]*\"|'[^']*')"),
        QRegularExpression::CaseInsensitiveOption);
    result.remove(onEventAttr);

    // 6. Replace javascript: href/src values with '#'
    static const QRegularExpression jsHref(
        QStringLiteral("(href|src)\\s*=\\s*\"javascript:[^\"]*\""),
        QRegularExpression::CaseInsensitiveOption);
    result.replace(jsHref, QStringLiteral("href=\"#\""));

    return result;
}
