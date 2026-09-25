// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file help-service.cpp
 * @brief Implementation of HelpService, which pre-loads the bundled
 *        user-doc markdown files at construction; reads via QFile
 *        against the QRC paths `backend/CMakeLists.txt` registers.
 * @layer Service
 * @dependencies Qt6::Core (QFile, QResource).
 */

#include "help-service.h"

#include "../utils/logger.h"

#include <QTextDocument>

#include <QFile>
#include <QRegularExpression>
#include <QStringLiteral>
#include <QVariantMap>

namespace {

// QML resource prefix the qt_add_qml_module pipeline produces for our
// module (`org.verzeta.studio`). The 12 user-doc .md files are aliased
// under user-docs/<name> per `backend/CMakeLists.txt` so they resolve
// to `:/qt/qml/org/verzeta/studio/user-docs/<file>` once bundled.
constexpr auto kQrcPrefix = ":/qt/qml/org/verzeta/studio/user-docs/";

constexpr auto kCssPrelude = R"(
h1 { margin-top: 24px; margin-bottom: 14px; }
h2 { margin-top: 22px; margin-bottom: 12px; }
h3 { margin-top: 18px; margin-bottom: 10px; }
h4 { margin-top: 14px; margin-bottom:  8px; }
p  { margin-top:  0;   margin-bottom: 10px; line-height: 140%; }
li { margin-top:  0;   margin-bottom:  6px; line-height: 140%; }
ul, ol { margin-top: 4px; margin-bottom: 12px; }
table { margin-top: 10px; margin-bottom: 12px; }
th, td { padding: 4px 10px; }
pre { padding: 8px; line-height: 130%; }
blockquote { margin: 8px 0; padding-left: 12px; }
)";

}  // namespace

QString HelpService::loadBody(const QString& file) {
    const QString path = QString::fromLatin1(kQrcPrefix) + file;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qCWarning(verzetaUi) << "HelpService: failed to open bundled doc at" << path << "—"
                             << f.errorString();
        return {};
    }
    const QByteArray bytes = f.readAll();
    f.close();
    const QString markdown = QString::fromUtf8(bytes);

    // setDefaultStyleSheet is applied BEFORE setMarkdown so the CSS
    // rules land in the document's <style> block when toHtml() runs.
    // BUT: Qt 6's toHtml() also emits explicit inline
    // `style="margin-top:0; margin-bottom:0; …"` on every <p> / <h*>
    // / <li> element. Inline CSS has higher specificity than the
    // <style> block, so the prelude's spacing rules are silently
    // overridden. Below we post-process the HTML to inject explicit
    // `<br/>` elements after each header and around lists — visual
    // breathing room that Qt's HTML renderer honours reliably,
    // independent of its limited CSS subset.
    QTextDocument doc;
    doc.setDefaultStyleSheet(QString::fromLatin1(kCssPrelude));
    doc.setMarkdown(markdown);
    QString html = doc.toHtml();

    // Insert a `<br/>` BEFORE every <h1..h6> opening tag so the gap
    // appears between the PREVIOUS section's content and the NEXT
    // section's heading (the "polished doc" reading rhythm:
    // [content A] [gap] [heading B] [content B]). The earlier
    // attempt put the `<br/>` after the closing </hN> which produced
    // the opposite — gap inserted between a heading and its own
    // content, leaving the previous section flush against the next
    // section's heading.
    static const QRegularExpression kHeaderOpen(QStringLiteral("<(h[1-6])(\\s|>)"));
    html.replace(kHeaderOpen, QStringLiteral("<br/><\\1\\2"));

    // Strip the leading <br/> the rule above would otherwise produce
    // ABOVE the very first heading in the document — no gap is wanted
    // at the very top of the rendered body.
    static const QRegularExpression kLeadingBr(QStringLiteral("(<body[^>]*>\\s*)<br/>"));
    html.replace(kLeadingBr, QStringLiteral("\\1"));

    // Strip Qt's hardcoded color choices so TextEdit's `color:`
    // property (Kirigami.Theme.textColor on the QML side) drives the
    // rendering colour. Qt's `toHtml()` bakes light-palette defaults
    // into inline `color:#xxxxxx` / `bgcolor="..."` declarations which
    // render invisibly on the app's dark theme.
    static const QRegularExpression kInlineColor(QStringLiteral("color\\s*:\\s*[^;\"]+;?"));
    html.replace(kInlineColor, QString());
    static const QRegularExpression kBgcolorAttr(QStringLiteral("\\s+bgcolor\\s*=\\s*\"[^\"]*\""));
    html.replace(kBgcolorAttr, QString());
    static const QRegularExpression kInlineBgColor(
        QStringLiteral("background-color\\s*:\\s*[^;\"]+;?"));
    html.replace(kInlineBgColor, QString());

    return html;
}

HelpService::HelpService(QObject* parent) : QObject(parent) {
    // Catalog order = sidebar order. Sections group related docs so the
    // sidebar reads top-to-bottom as a real getting-started → daily-use
    // → troubleshoot flow, not an alphabetical dump.
    //
    // Icons: every entry carries a (primary, fallback) pair. The
    // primary is the most-descriptive standard Freedesktop icon name;
    // the fallback is a guaranteed-Breeze-baseline name used by
    // Kirigami.Icon's `fallback:` property when the active icon theme
    // (Breeze on Linux, system default elsewhere) doesn't ship the
    // primary. Both names are from the Freedesktop Icon Naming Spec
    // baseline shipped by every major theme — chosen so an early
    // pre-rendered HelpOverlay doesn't show empty boxes for missing
    // icons.
    const QList<Entry> seeds = {
        {"00",
         QStringLiteral("Introduction"),
         QStringLiteral("start-here"),
         QStringLiteral("help-contents"),
         QStringLiteral("Get Started"),
         QStringLiteral("00-introduction.md"),
         {}},
        {"01",
         QStringLiteral("Quick Start"),
         QStringLiteral("media-playback-start"),
         QStringLiteral("go-next"),
         QStringLiteral("Get Started"),
         QStringLiteral("01-quick-start.md"),
         {}},
        {"02",
         QStringLiteral("Providers"),
         QStringLiteral("network-server"),
         QStringLiteral("applications-internet"),
         QStringLiteral("Get Started"),
         QStringLiteral("02-providers.md"),
         {}},

        {"03",
         QStringLiteral("Conversations"),
         QStringLiteral("mail-message"),
         QStringLiteral("dialog-messages"),
         QStringLiteral("Core Features"),
         QStringLiteral("03-conversations.md"),
         {}},
        {"04",
         QStringLiteral("Multi-Agent Teams"),
         QStringLiteral("system-users"),
         QStringLiteral("user-group-new"),
         QStringLiteral("Core Features"),
         QStringLiteral("04-multi-agent-teams.md"),
         {}},
        {"05",
         QStringLiteral("Projects"),
         QStringLiteral("folder"),
         QStringLiteral("folder-blue"),
         QStringLiteral("Core Features"),
         QStringLiteral("05-projects.md"),
         {}},
        {"06",
         QStringLiteral("Canvas and Tasks"),
         QStringLiteral("document-edit"),
         QStringLiteral("accessories-text-editor"),
         QStringLiteral("Core Features"),
         QStringLiteral("06-canvas-and-tasks.md"),
         {}},
        {"07",
         QStringLiteral("Skills"),
         QStringLiteral("applications-system"),
         QStringLiteral("preferences-system"),
         QStringLiteral("Core Features"),
         QStringLiteral("07-skills.md"),
         {}},
        {"08",
         QStringLiteral("Remote and Android"),
         QStringLiteral("network-wireless"),
         QStringLiteral("network-connect"),
         QStringLiteral("Core Features"),
         QStringLiteral("08-remote-android.md"),
         {}},
        {"09",
         QStringLiteral("Activity Timeline"),
         QStringLiteral("view-history"),
         QStringLiteral("view-list-details"),
         QStringLiteral("Core Features"),
         QStringLiteral("09-activity-timeline.md"),
         {}},

        {"10",
         QStringLiteral("Troubleshooting"),
         QStringLiteral("dialog-warning"),
         QStringLiteral("dialog-information"),
         QStringLiteral("Help"),
         QStringLiteral("10-troubleshooting.md"),
         {}},
        {"11",
         QStringLiteral("FAQ"),
         QStringLiteral("dialog-question"),
         QStringLiteral("help-about"),
         QStringLiteral("Help"),
         QStringLiteral("11-faq.md"),
         {}},
    };

    m_entries.reserve(seeds.size());
    for (const Entry& seed : seeds) {
        Entry e = seed;
        e.body = loadBody(e.file);
        m_indexById.insert(e.id, m_entries.size());
        m_entries.append(e);
    }

    int loaded = 0;
    for (const Entry& e : m_entries) {
        if (!e.body.isEmpty())
            ++loaded;
    }
    qCInfo(verzetaUi) << "HelpService: loaded" << loaded << "of" << m_entries.size()
                      << "bundled user-doc files";
}

HelpService::~HelpService() = default;

QVariantList HelpService::docs() const {
    QVariantList out;
    out.reserve(m_entries.size());
    for (const Entry& e : m_entries) {
        QVariantMap row;
        row.insert(QStringLiteral("id"), e.id);
        row.insert(QStringLiteral("title"), e.title);
        row.insert(QStringLiteral("iconName"), e.iconName);
        row.insert(QStringLiteral("iconFallback"), e.iconFallback);
        row.insert(QStringLiteral("section"), e.section);
        row.insert(QStringLiteral("file"), e.file);
        out.append(row);
    }
    return out;
}

QString HelpService::body(const QString& docId) const {
    const int idx = m_indexById.value(docId, -1);
    if (idx < 0 || idx >= m_entries.size()) {
        qCWarning(verzetaUi) << "HelpService::body: unknown docId" << docId;
        return {};
    }
    return m_entries.at(idx).body;
}

QString HelpService::titleFor(const QString& docId) const {
    const int idx = m_indexById.value(docId, -1);
    if (idx < 0 || idx >= m_entries.size())
        return {};
    return m_entries.at(idx).title;
}
