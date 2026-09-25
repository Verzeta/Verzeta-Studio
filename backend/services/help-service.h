// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file help-service.h
 * @brief Tiny QObject that owns the in-app user-documentation
 *        catalog and exposes the bundled markdown to QML.
 * @layer Service
 * @dependencies Qt6::Core (QFile + QResource for the bundled .md reads).
 */


#pragma once

#include <QHash>
#include <QObject>
#include <QString>
#include <QVariantList>

/**
 * @brief In-app help / user-documentation provider.
 *
 * Owned by `AppController` via `std::unique_ptr<HelpService>`.
 * Construction is zero-argument (engine-layer dependency-free) and
 * pre-loads every doc named in the catalog from its QRC entry,
 * converting each one from Markdown to a CSS-styled HTML string so
 * the QML viewer renders headers / paragraphs / lists / tables with
 * proper visual spacing. (Qt's TextEdit MarkdownText renderer is
 * very compact by default; using RichText with a CSS prelude is the
 * only reliable way to get readable spacing across themes.)
 *
 * QML exposure: registered as the `Help` singleton in
 * `AppController::registerTypes()`. `HelpOverlay.qml` reads:
 *   - `Help.docs()`:            the doc catalog (renders as the sidebar).
 *   - `Help.body(docId)`:       pre-cached styled HTML for one doc.
 *   - `Help.titleFor(docId)`:   small convenience for header captions.
 */
class HelpService : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Constructs the service.
     * @param parent Optional Qt parent.
     */
    explicit HelpService(QObject* parent = nullptr);
    ~HelpService() override;

    /**
     * @brief The full doc catalog, in reading order.
     *
     *        Each entry is a QVariantMap with: id (stable lookup key),
     *        title (sidebar / header caption), iconName (KDE icon
     *        name), iconFallback (KDE icon name fallback), section
     *        (sidebar section header), file (basename under
     *        Documentation/User/, diagnostics only).
     * @returns List of doc-row maps in reading order.
     */
    Q_INVOKABLE QVariantList docs() const;

    /**
     * @brief CSS-styled HTML body for the doc with the given id.
     *        Consumed by HelpOverlay's TextEdit with `textFormat:
     *        TextEdit.RichText`.
     * @param docId Doc identifier from `docs()`.
     * @returns HTML body, or empty when the id is unknown or the
     *          resource read failed at startup.
     */
    Q_INVOKABLE QString body(const QString& docId) const;

    /**
     * @brief Display title for the doc with the given id. Saves QML
     *        from re-searching `docs()` when it already has an id in
     *        hand.
     * @param docId Doc identifier.
     * @returns Title text, or empty when the id is unknown.
     */
    Q_INVOKABLE QString titleFor(const QString& docId) const;

  private:
    /**
     * @brief Catalog row, kept in a vector so iteration order is
     *        the reading order shown in the sidebar.
     */
    struct Entry {
        QString id;
        QString title;
        QString iconName;
        QString iconFallback;
        QString section;
        QString file;
        QString body;  ///< Pre-rendered CSS-styled HTML; empty on read failure.
    };

    /**
     * @brief Read one doc's markdown from the QRC, convert via
     *        `QTextDocument::setMarkdown()` with a CSS prelude that
     *        gives headers / paragraphs / lists / tables proper
     *        spacing, and return the resulting HTML string. The CSS
     *        intentionally specifies NO colours so the QML side's
     *        Kirigami.Theme palette wins for every glyph.
     */
    static QString loadBody(const QString& file);

    QList<Entry> m_entries;
    QHash<QString, int> m_indexById;  ///< id → m_entries index, O(1) lookup.
};
