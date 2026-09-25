// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file skill-parser.cpp
 * @brief Implementation of the SkillParser routines.  See header for
 *        contract.
 * @layer Utility
 * @dependencies Qt6::Core only.  Frontmatter parsing is done with a
 *               minimal hand-rolled YAML-ish reader rather than
 *               pulling in libyaml, because frontmatter is a small subset
 *               (quoted strings, simple scalars, flow lists).
 */

#include "skill-parser.h"

#include <QTextStream>

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMimeDatabase>
#include <QRegularExpression>

namespace SkillParser {

namespace {

// Size caps
constexpr qint64 kSkillMdMaxBytes = 256 * 1024;           // 256 KiB
constexpr qint64 kSupportingFileMaxBytes = 1024 * 1024;   // 1 MiB
constexpr qint64 kFolderTotalMaxBytes = 8 * 1024 * 1024;  // 8 MiB
constexpr int kExcerptMaxLen = 120;

/**
 * @brief Named suspicious-marker regex used by the file scanner.
 *
 * The scanner runs each entry's regex over every text file in the
 * candidate skill folder; a match emits a warning with the entry's
 * `name` so log filtering / triage downstream can address each
 * marker class independently.
 */
struct WarnRegex {
    QString name;           ///< Stable identifier (e.g. WARN_PIPE_TO_SHELL).
    QRegularExpression re;  ///< Pre-compiled regex pattern.
};

// Build once, reused across all parseSkillFolder calls. Lazily
// initialised on first use; mutex-free because Qt6 QRegularExpression
// is thread-safe for matching after compilation.
const QList<WarnRegex>& warnRegexes() {
    static const QList<WarnRegex> kSet = {
        {QStringLiteral("WARN_PIPE_TO_SHELL"),
         QRegularExpression(
             QStringLiteral(
                 R"(\b(curl|wget|fetch)\b[^\n]{0,200}\|\s*(sh|bash|zsh|fish|sudo|exec|eval)\b)"),
             QRegularExpression::CaseInsensitiveOption)},
        {QStringLiteral("WARN_CREDENTIAL_PATH"),
         QRegularExpression(
             QStringLiteral(
                 R"(~/\.(ssh|aws|gnupg|kube|gnome-keyring|mozilla|config/google-chrome|config/chromium|netrc|docker))"),
             QRegularExpression::CaseInsensitiveOption)},
        {QStringLiteral("WARN_BASE64_EXEC"),
         QRegularExpression(
             QStringLiteral(
                 R"(\bbase64\s+(-d|--decode|-D)\b[^\n]{0,200}\|\s*(sh|bash|exec|eval|python|node))"),
             QRegularExpression::CaseInsensitiveOption)},
        {QStringLiteral("WARN_PROMPT_OVERRIDE"),
         QRegularExpression(
             QStringLiteral(
                 R"(\b(ignore|disregard|override|bypass)\s+(?:[^\n]{0,40}\b)?(system|developer|user|prior)\s+(prompt|instructions?|rules?|message))"),
             QRegularExpression::CaseInsensitiveOption)},
        {QStringLiteral("WARN_EXFIL_NETWORK"),
         QRegularExpression(
             QStringLiteral(
                 R"(\b(curl|wget|fetch|nc|netcat)\b[^\n]{0,200}\b(post|put|--data|-d)\b)"),
             QRegularExpression::CaseInsensitiveOption)},
        {QStringLiteral("WARN_CRYPTO_WALLET"),
         QRegularExpression(QStringLiteral(
             R"(\b(0x[a-fA-F0-9]{40}|bc1[a-z0-9]{25,90}|[13][a-km-zA-HJ-NP-Z1-9]{25,34})\b)"))},
        {QStringLiteral("WARN_PRIVATE_KEY"),
         QRegularExpression(QStringLiteral(
             R"(-----BEGIN\s+(RSA|EC|DSA|OPENSSH|PRIVATE)\s+(PRIVATE\s+)?KEY-----)"))},
    };
    return kSet;
}

// Text-file detection: match by MIME starts with "text/" or by the
// known-text extension allowlist. We deliberately avoid scanning
// binaries (images, PDFs, fonts) — regex over those is noise.
bool isTextFile(const QString& absPath) {
    static const QSet<QString> kTextExt = {
        QStringLiteral("md"),
        QStringLiteral("txt"),
        QStringLiteral("sh"),
        QStringLiteral("py"),
        QStringLiteral("js"),
        QStringLiteral("ts"),
        QStringLiteral("json"),
        QStringLiteral("yaml"),
        QStringLiteral("yml"),
        QStringLiteral("toml"),
        QStringLiteral("ini"),
        QStringLiteral("cfg"),
        QStringLiteral("conf"),
        QStringLiteral("xml"),
        QStringLiteral("html"),
        QStringLiteral("css"),
        QStringLiteral("svg"),
    };
    const QString ext = QFileInfo(absPath).suffix().toLower();
    if (kTextExt.contains(ext))
        return true;

    // Fallback: MIME-type sniff. Cheap (looks at extension first then
    // first ~4 KiB).
    static QMimeDatabase db;
    const QString mime = db.mimeTypeForFile(absPath).name();
    return mime.startsWith(QStringLiteral("text/"));
}

// Hand-rolled YAML-ish frontmatter parser. We intentionally support
// only the documented subset: quoted strings, simple
// scalars, flow-style sequences ([a, b, c]). Block-style sequences
// and nested maps are NOT supported. The plan calls this out as a
// conservative subset; richer parsing is a follow-up.
QHash<QString, QString> stringFields;
QHash<QString, QStringList> listFields;

void resetFields() {
    stringFields.clear();
    listFields.clear();
}

QString stripQuotes(QString v) {
    v = v.trimmed();
    if (v.length() >= 2) {
        const QChar f = v.front(), b = v.back();
        if ((f == QLatin1Char('"') && b == QLatin1Char('"')) ||
            (f == QLatin1Char('\'') && b == QLatin1Char('\''))) {
            return v.mid(1, v.length() - 2);
        }
    }
    return v;
}

QStringList parseFlowList(const QString& raw) {
    // Expected form: [a, "b c", item3]
    QString inner = raw.trimmed();
    if (inner.startsWith(QLatin1Char('[')) && inner.endsWith(QLatin1Char(']'))) {
        inner = inner.mid(1, inner.length() - 2);
    }
    QStringList out;
    QString cur;
    bool inQuote = false;
    QChar quoteCh;
    for (QChar c : inner) {
        if (inQuote) {
            if (c == quoteCh) {
                inQuote = false;
            } else {
                cur.append(c);
            }
        } else if (c == QLatin1Char('"') || c == QLatin1Char('\'')) {
            inQuote = true;
            quoteCh = c;
        } else if (c == QLatin1Char(',')) {
            const QString t = cur.trimmed();
            if (!t.isEmpty())
                out.append(t);
            cur.clear();
        } else {
            cur.append(c);
        }
    }
    const QString t = cur.trimmed();
    if (!t.isEmpty())
        out.append(t);
    return out;
}

/**
 * Parse YAML-ish frontmatter from `mdContent` if it's present.
 * Frontmatter is OPTIONAL; many real-world ClawHub skills ship
 * without it. The caller is responsible for filling missing
 * `name` / `description` from fallbacks (folder basename, first
 * heading) when this returns success-with-empty-fields.
 *
 * Tolerates:
 *   - BOM + leading whitespace / blank lines before the opening fence
 *   - `\r\n` line endings
 *   - Missing fence entirely (returns success, empty fields)
 *   - Opening `---` without a closing fence (logs a soft warning,
 *     skips frontmatter entirely)
 */
bool parseFrontmatter(const QString& mdContent, QString& outError) {
    Q_UNUSED(outError);
    resetFields();

    // Step 1: skip BOM + leading whitespace / blank lines.
    int p = 0;
    if (p < mdContent.size() && mdContent.at(p) == QChar(0xFEFF))
        ++p;
    while (p < mdContent.size() &&
           (mdContent.at(p).isSpace() || mdContent.at(p) == QLatin1Char('\n'))) {
        ++p;
    }

    // Step 2: detect an opening `---` fence on its own line.
    //   "---\n" or "---\r\n" — anything else means no frontmatter.
    const bool hasOpenFence = (mdContent.mid(p, 4) == QStringLiteral("---\n")) ||
                              (mdContent.mid(p, 5) == QStringLiteral("---\r\n"));
    if (!hasOpenFence) {
        // Frontmatter is optional. parseSkillFolder fills the gaps
        // from the folder basename + first markdown heading.
        return true;
    }

    // Step 3: locate the closing `---` fence. If absent → silently
    // skip (treat as if no frontmatter were present), don't reject
    // the whole skill.
    const int afterOpenFence = mdContent.indexOf(QLatin1Char('\n'), p) + 1;
    const int closeIdx = mdContent.indexOf(QStringLiteral("\n---"), afterOpenFence);
    if (closeIdx < 0) {
        // Malformed frontmatter — keep the skill but skip metadata.
        return true;
    }
    const QString block = mdContent.mid(afterOpenFence, closeIdx - afterOpenFence);

    // Step 4: parse the conservative key:value subset.
    const QStringList lines = block.split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    for (const QString& rawLine : lines) {
        QString line = rawLine;
        if (line.endsWith(QLatin1Char('\r')))
            line.chop(1);
        if (line.trimmed().startsWith(QLatin1Char('#')))
            continue;
        if (line.trimmed().isEmpty())
            continue;

        const int colon = line.indexOf(QLatin1Char(':'));
        if (colon < 0)
            continue;  // Block-style "  - foo" not supported.
        const QString key = line.left(colon).trimmed();
        const QString val = line.mid(colon + 1).trimmed();

        if (val.startsWith(QLatin1Char('['))) {
            listFields.insert(key, parseFlowList(val));
        } else {
            stringFields.insert(key, stripQuotes(val));
        }
    }
    return true;
}

/**
 * Pull the first usable description string out of a SKILL.md body.
 * Prefers the first ATX heading ("# Heading"), then the first
 * non-blank prose line. Returns empty string when nothing usable
 * is found.
 */
QString extractFirstHeadingOrLine(const QString& mdContent) {
    const QStringList lines = mdContent.split(QLatin1Char('\n'));
    QString firstLine;
    for (const QString& raw : lines) {
        QString l = raw;
        if (l.endsWith(QLatin1Char('\r')))
            l.chop(1);
        const QString t = l.trimmed();
        if (t.isEmpty())
            continue;
        if (t.startsWith(QStringLiteral("---")))
            continue;  // fence
        // Skip frontmatter key-value lines that may have leaked through.
        if (t.contains(QLatin1Char(':')) && !t.startsWith(QLatin1Char('#')) &&
            firstLine.isEmpty() && lines.indexOf(raw) < 20) {
            // Heuristic: top-of-file colon lines are likely frontmatter
            // key-values; only take them when nothing else turns up.
        }
        if (t.startsWith(QLatin1Char('#'))) {
            // ATX heading — strip the leading #s + space.
            QString h = t;
            while (h.startsWith(QLatin1Char('#')))
                h.remove(0, 1);
            return h.trimmed();
        }
        if (firstLine.isEmpty())
            firstLine = t;
    }
    return firstLine;
}

}  // namespace

bool isValidSkillId(const QString& candidate) {
    static const QRegularExpression kIdRe(QStringLiteral(R"(^[a-z0-9][a-z0-9_-]{0,63}$)"));
    return kIdRe.match(candidate).hasMatch();
}

namespace {

/**
 * @brief Derive a grammar-valid skill id from an arbitrary string.
 *
 *        Lower-cases, replaces any run of characters outside the id
 *        grammar (`[a-z0-9_-]`) with a single hyphen, strips leading /
 *        trailing hyphens and underscores (the grammar requires the
 *        first char be `[a-z0-9]`), and caps the result at 64 chars.
 *        Returns an empty string when nothing slug-able remains.
 *
 * @param input  Any string (a frontmatter `name`, slug hint, or folder
 *               basename).
 * @returns A string satisfying isValidSkillId(), or empty if none could
 *          be formed.
 */
QString slugifySkillId(const QString& input) {
    QString s = input.toLower();
    static const QRegularExpression nonId(QStringLiteral(R"([^a-z0-9_-]+)"));
    s.replace(nonId, QStringLiteral("-"));
    static const QRegularExpression edges(QStringLiteral(R"(^[-_]+|[-_]+$)"));
    s.replace(edges, QString());
    if (s.size() > 64) {
        s = s.left(64);
        // left(64) can re-expose a trailing run; trailing -/_ is legal
        // grammar so no further trim is required.
    }
    return s;
}

}  // namespace

ParseResult parseSkillFolder(const QString& folderPath, const QString& idHint) {
    ParseResult res;

    QFileInfo rootInfo(folderPath);
    if (!rootInfo.exists() || !rootInfo.isDir()) {
        res.error = QStringLiteral("not a directory: %1").arg(folderPath);
        return res;
    }
    const QString rootCanon = rootInfo.canonicalFilePath();

    // 1) SKILL.md must exist at root.
    const QString skillMdPath = rootCanon + QStringLiteral("/SKILL.md");
    QFile skillMd(skillMdPath);
    if (!skillMd.exists()) {
        res.error = QStringLiteral("SKILL.md not found at skill root");
        return res;
    }
    const QFileInfo skillMdInfo(skillMdPath);
    if (skillMdInfo.size() > kSkillMdMaxBytes) {
        res.error = QStringLiteral("SKILL.md exceeds %1-byte cap").arg(kSkillMdMaxBytes);
        return res;
    }
    if (!skillMd.open(QIODevice::ReadOnly | QIODevice::Text)) {
        res.error = QStringLiteral("cannot open SKILL.md: %1").arg(skillMd.errorString());
        return res;
    }
    const QString mdContent = QString::fromUtf8(skillMd.readAll());
    skillMd.close();

    // 2) Frontmatter parse — best-effort. Real-world ClawHub skills
    //    often ship without YAML frontmatter, so the parser tolerates
    //    its absence and we fill in defaults from the folder basename
    //    (the slug — which ClawHub guarantees matches our id grammar)
    //    and from the first markdown heading in the body.
    QString fmError;
    parseFrontmatter(mdContent, fmError);

    const QString nameVal = stringFields.value(QStringLiteral("name"));
    QString descVal = stringFields.value(QStringLiteral("description"));
    const QString verVal = stringFields.value(QStringLiteral("version"));
    const QString sourceVal = stringFields.value(QStringLiteral("source"));
    const QString sourceUrlVal = stringFields.value(QStringLiteral("source_url"));
    const QStringList tagsVal = listFields.value(QStringLiteral("tags"));
    const QStringList toolsVal = listFields.value(QStringLiteral("tools"));

    // Derive the skill id. The frontmatter `name` is a DISPLAY name —
    // real-world SKILL.md files set it to a human string ("PDF Form
    // Filler") that does not satisfy the id grammar — so it must NOT be
    // forced through the grammar as the id (that rejected most catalog
    // skills outright). Priority:
    //   1. The caller's slug hint when it is already grammar-valid
    //      (ClawHub passes the catalog slug here — it is the canonical
    //      identity used for reinstall/update, so it wins over `name`).
    //   2. A frontmatter `name` that itself satisfies the grammar
    //      (legacy skills whose `name` already IS the slug).
    //   3. The slugified frontmatter `name`.
    //   4. The slugified slug hint (a non-grammar hint, rare).
    //   5. The slugified folder basename (user folder imports / ZIPs
    //      that wrap content under a single top-level dir).
    QString idVal;
    if (!idHint.isEmpty() && isValidSkillId(idHint)) {
        idVal = idHint;
    } else if (!nameVal.isEmpty() && isValidSkillId(nameVal)) {
        idVal = nameVal;
    } else if (!nameVal.isEmpty()) {
        idVal = slugifySkillId(nameVal);
    } else if (!idHint.isEmpty()) {
        idVal = slugifySkillId(idHint);
    } else {
        idVal = slugifySkillId(QFileInfo(rootCanon).fileName());
    }

    // Fallback: first heading or first non-blank line as the
    // description. Used for the AVAILABLE SKILLS prompt layer + the
    // SkillsPage row text.
    if (descVal.isEmpty()) {
        descVal = extractFirstHeadingOrLine(mdContent);
    }
    if (descVal.isEmpty()) {
        descVal = QStringLiteral("(no description)");
    }

    // 3) ID grammar — only fails now when NOTHING slug-able could be
    //    derived from the slug hint, the frontmatter `name`, or the
    //    folder basename (e.g. a name of only punctuation / non-Latin
    //    script with no idHint). Normal human-readable names are
    //    slugified above, not rejected.
    if (!isValidSkillId(idVal)) {
        res.error = QStringLiteral("could not derive a valid skill id (grammar "
                                   "[a-z0-9][a-z0-9_-]{0,63}) from the skill's "
                                   "name or folder. Add a `name:` line to "
                                   "SKILL.md frontmatter using letters / digits "
                                   "/ hyphens.");
        return res;
    }

    // The human-readable display name is the frontmatter `name` when
    // present; otherwise fall back to the (slug) id so the UI always has
    // a label.
    const QString displayVal = nameVal.isEmpty() ? idVal : nameVal;

    // 4) Walk all files. Reject symlinks anywhere. Enforce per-file +
    //    total caps. Accumulate text-file list for warning scan.
    qint64 totalBytes = 0;
    QStringList textFilesAbs;
    QDirIterator it(rootCanon, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString abs = it.next();
        const QFileInfo fi(abs);
        if (fi.isSymLink()) {
            res.error = QStringLiteral("symbolic links are not allowed in skills: %1")
                            .arg(QDir(rootCanon).relativeFilePath(abs));
            return res;
        }
        if (fi.size() > kSupportingFileMaxBytes) {
            res.error = QStringLiteral("file %1 exceeds %2-byte cap")
                            .arg(QDir(rootCanon).relativeFilePath(abs))
                            .arg(kSupportingFileMaxBytes);
            return res;
        }
        totalBytes += fi.size();
        if (totalBytes > kFolderTotalMaxBytes) {
            res.error =
                QStringLiteral("skill folder exceeds %1-byte total cap").arg(kFolderTotalMaxBytes);
            return res;
        }
        if (isTextFile(abs))
            textFilesAbs.append(abs);
    }

    // 5) Suspicious-marker scan over every text file.
    const QList<WarnRegex>& regexes = warnRegexes();
    for (const QString& abs : textFilesAbs) {
        QFile f(abs);
        if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
            continue;
        QTextStream ts(&f);
        const QString rel = QDir(rootCanon).relativeFilePath(abs);
        int lineNo = 0;
        while (!ts.atEnd()) {
            ++lineNo;
            const QString line = ts.readLine();
            for (const WarnRegex& wr : regexes) {
                const auto m = wr.re.match(line);
                if (!m.hasMatch())
                    continue;
                SkillWarning w;
                w.regexName = wr.name;
                w.fileRelativePath = rel;
                w.lineNumber = lineNo;
                QString excerpt = m.captured(0);
                if (excerpt.size() > kExcerptMaxLen) {
                    excerpt = excerpt.left(kExcerptMaxLen) + QStringLiteral("…");
                }
                w.matchedExcerpt = excerpt;
                res.warnings.append(w);
            }
        }
        f.close();
    }

    // 6) Populate the Skill POD (installPath / installedAtMs filled by
    //    SkillService after the parser succeeds).
    Skill& s = res.skill;
    s.id = idVal;
    s.displayName = displayVal;
    s.description = descVal;
    s.version = verVal;
    s.source = sourceVal.isEmpty() ? QStringLiteral("manual") : sourceVal;
    s.sourceUrl = sourceUrlVal;
    s.tags = tagsVal;
    s.declaredTools = toolsVal;
    s.warnings = res.warnings;
    s.reviewState = QStringLiteral("unreviewed");

    res.success = true;
    return res;
}

}  // namespace SkillParser
