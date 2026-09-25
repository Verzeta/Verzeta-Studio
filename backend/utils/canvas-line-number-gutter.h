// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file canvas-line-number-gutter.h
 * @brief C++ painted line-number gutter for the canvas editor.
 *        Replaces the QML `Repeater` + per-line `Item` / `Label`
 *        approach (slow + bind-storming on every keystroke because
 *        each delegate listened to TextArea's `cursorRectangle`).
 *
 *        This component paints line numbers directly from the
 *        QTextDocument layout (block-by-block iteration through
 *        `QAbstractTextDocumentLayout::blockBoundingRect`) and
 *        repaints on three signals: documentSizeChanged (block count
 *        / wrap), scrollYChanged (TextArea's contentY), and
 *        font / colour changes.
 *
 * @layer Frontend UI (QML-exposed C++ component)
 * @dependencies Qt6::Quick (QQuickPaintedItem, QQuickTextDocument),
 *               Qt6::Gui (QPainter, QFontMetrics).
 *
 * Registered in `AppController::registerTypes()` as the
 * `CanvasLineNumberGutter` QML type, matching the existing
 * `SyntaxHighlighter` registration pattern.
 *
 * Usage from QML:
 *
 *   CanvasLineNumberGutter {
 *       width: 56
 *       height: parent.height
 *       textDocument: bodyTextArea.textDocument
 *       scrollY: bodyTextArea.contentY
 *       font: bodyTextArea.font
 *       textColor: Kirigami.Theme.disabledTextColor
 *       backgroundColor: Qt.darker(Kirigami.Theme.backgroundColor, 1.06)
 *   }
 *
 * Threading: paints on the GUI thread (Qt Quick render loop runs the
 * paint method on the main thread for QQuickPaintedItem). All inputs
 * are owned by the QML scene graph, so no cross-thread coordination
 * is needed.
 */


#pragma once

#include <QColor>
#include <QFont>
#include <QPointer>
#include <QQuickPaintedItem>
#include <QQuickTextDocument>

class QTextDocument;

/**
 * @brief QML-exposed painted gutter that renders 1-based line numbers
 *        beside a TextArea editor.
 *
 * Owns no document of its own. It binds to the QTextDocument of an
 * external editor and re-paints when the document changes, the
 * scroll position changes, or the font / colour properties change.
 * Sized + positioned by the QML scene graph.
 */
class CanvasLineNumberGutter : public QQuickPaintedItem {
    Q_OBJECT

    Q_PROPERTY(QQuickTextDocument* textDocument READ textDocument WRITE setTextDocument NOTIFY
                   textDocumentChanged)  ///< QML text-document handle whose lines are numbered.
    Q_PROPERTY(
        qreal scrollY READ scrollY WRITE setScrollY NOTIFY
            scrollYChanged)  ///< Editor's vertical scroll offset (matches TextArea contentY).
    Q_PROPERTY(
        QFont font READ font WRITE setFont NOTIFY
            fontChanged)  ///< Font used to render the line numbers (typically matches editor font).
    Q_PROPERTY(QColor textColor READ textColor WRITE setTextColor NOTIFY
                   textColorChanged)  ///< Colour the line numbers are painted in.
    Q_PROPERTY(QColor backgroundColor READ backgroundColor WRITE setBackgroundColor NOTIFY
                   backgroundColorChanged)  ///< Background fill colour (transparent by default).
    Q_PROPERTY(int rightPadding READ rightPadding WRITE setRightPadding NOTIFY
                   rightPaddingChanged)  ///< Pixels of padding between the rightmost digit and the
                                         ///< right edge.

  public:
    /**
     * @brief Constructs the gutter.
     * @param parent  Optional QQuickItem parent (typically owned by the QML scene).
     */
    explicit CanvasLineNumberGutter(QQuickItem* parent = nullptr);

    /** @brief Destroys the gutter and disconnects any active document signal subscriptions. */
    ~CanvasLineNumberGutter() override;

    /** @brief Returns the currently-bound QML text document.
     *  @returns The QQuickTextDocument pointer, or nullptr when no document is bound. */
    QQuickTextDocument* textDocument() const;

    /** @brief Binds a new QML text document.  Rewires signal subscriptions and triggers a repaint.
     *  @param doc  The document to bind; may be nullptr to clear the binding. */
    void setTextDocument(QQuickTextDocument* doc);

    /** @brief Returns the current scroll offset.
     *  @returns The vertical scroll offset in pixels. */
    qreal scrollY() const;

    /** @brief Sets the scroll offset and triggers a repaint when the value changes.
     *  @param y  The new vertical scroll offset in pixels. */
    void setScrollY(qreal y);

    /** @brief Returns the current paint font.
     *  @returns The QFont used to render line numbers. */
    QFont font() const;

    /** @brief Sets a new paint font and triggers a repaint when the value changes.
     *  @param f  The new QFont. */
    void setFont(const QFont& f);

    /** @brief Returns the current line-number text colour.
     *  @returns The QColor used to paint the digits. */
    QColor textColor() const;

    /** @brief Sets a new text colour and triggers a repaint when the value changes.
     *  @param c  The new QColor. */
    void setTextColor(const QColor& c);

    /** @brief Returns the current background fill colour.
     *  @returns The QColor painted behind the digits (transparent by default). */
    QColor backgroundColor() const;

    /** @brief Sets a new background fill colour and triggers a repaint when the value changes.
     *  @param c  The new QColor; alpha=0 leaves the gutter transparent. */
    void setBackgroundColor(const QColor& c);

    /** @brief Returns the current right-padding value.
     *  @returns The padding in pixels between the rightmost digit and the gutter's right edge. */
    int rightPadding() const;

    /** @brief Sets a new right-padding value and triggers a repaint when the value changes.
     *  @param p  The new padding in pixels (clamped to >= 0 by the setter). */
    void setRightPadding(int p);

    /**
     * @brief QQuickPaintedItem hook.  Iterates QTextDocument blocks,
     *        paints each block's 1-based line number right-aligned at
     *        the block's y-offset (minus scrollY).
     * @param painter  Scene-graph-supplied QPainter to issue draw calls against.
     */
    void paint(QPainter* painter) override;

  signals:
    /** @brief Emitted when setTextDocument() bound a new document. */
    void textDocumentChanged();
    /** @brief Emitted when setScrollY() changed the scroll offset. */
    void scrollYChanged();
    /** @brief Emitted when setFont() changed the paint font. */
    void fontChanged();
    /** @brief Emitted when setTextColor() changed the digit colour. */
    void textColorChanged();
    /** @brief Emitted when setBackgroundColor() changed the background fill. */
    void backgroundColorChanged();
    /** @brief Emitted when setRightPadding() changed the right-padding value. */
    void rightPaddingChanged();

  private slots:
    /** @brief Triggered by QTextDocument::contentsChanged or
     *         QAbstractTextDocumentLayout::documentSizeChanged. */
    void onDocumentChanged();

  private:
    /** Reconnect signal subscriptions after textDocument changes. */
    void rewireDocumentSignals();

    QPointer<QQuickTextDocument> m_qmlDoc;
    QPointer<QTextDocument> m_currentDoc;
    qreal m_scrollY = 0.0;
    QFont m_font;
    QColor m_textColor = QColor(150, 150, 150);
    QColor m_backgroundColor = QColor(0, 0, 0, 0);
    int m_rightPadding = 8;
};
