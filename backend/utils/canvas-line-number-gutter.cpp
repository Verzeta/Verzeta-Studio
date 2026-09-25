// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file canvas-line-number-gutter.cpp
 * @brief Implementation of CanvasLineNumberGutter, a QPainter-driven
 *        line-number gutter for the canvas editor. Replaces a slow
 *        QML Repeater that bound to TextArea's cursorRectangle.
 * @layer Frontend UI (QML-exposed C++ component)
 * @dependencies Qt6::Quick + Qt6::Gui.
 */

#include "canvas-line-number-gutter.h"

#include <QTextBlock>
#include <QTextDocument>

#include <QAbstractTextDocumentLayout>
#include <QFontMetricsF>
#include <QPainter>

CanvasLineNumberGutter::CanvasLineNumberGutter(QQuickItem* parent) : QQuickPaintedItem(parent) {
    // Default to a monospace font sized similarly to the Kirigami
    // default font; real value is set by the QML site via the font
    // property binding.
    m_font.setFamily(QStringLiteral("monospace"));
    m_font.setStyleHint(QFont::Monospace);
    m_font.setPointSize(10);

    // Render target: Image (Qt's default for text-heavy painted
    // items — the backing texture auto-sizes to
    // `item size × devicePixelRatio`, so fractional OS scaling
    // (1.25 / 1.5 / 1.75) renders crisply.
    //
    // The previous FramebufferObject target produced the "lines 1-9
    // sharp, lines 10+ stacked / doubled" corruption seen after
    // enabling system scaling: FBO targets allocate at logical-pixel
    // resolution regardless of DPR and accumulate stale texels when
    // the item is reflowed at a non-integer DPR. Image target solves
    // both — Qt scales the texture and clears it between paints.
    setRenderTarget(QQuickPaintedItem::Image);
    setAntialiasing(true);
}

CanvasLineNumberGutter::~CanvasLineNumberGutter() = default;

QQuickTextDocument* CanvasLineNumberGutter::textDocument() const {
    return m_qmlDoc.data();
}

void CanvasLineNumberGutter::setTextDocument(QQuickTextDocument* doc) {
    if (m_qmlDoc == doc)
        return;

    // Defensive: the previous manual `disconnect(m_currentDoc.data(),
    // ...)` could touch a half-destroyed QObject on rapid canvas
    // swaps / conversation switches. We rely on Qt's automatic
    // disconnect-on-sender-destruction instead. The
    // `Qt::UniqueConnection` flag in `rewireDocumentSignals` prevents
    // duplicate connections if the same document is re-attached.
    m_qmlDoc = doc;
    m_currentDoc = nullptr;  // reset before rewire
    rewireDocumentSignals();

    emit textDocumentChanged();
    update();
}

void CanvasLineNumberGutter::rewireDocumentSignals() {
    if (!m_qmlDoc) {
        m_currentDoc = nullptr;
        return;
    }

    QTextDocument* td = m_qmlDoc->textDocument();
    m_currentDoc = td;
    if (!td)
        return;

    // Repaint when content changes (block count + wrap layout) and
    // when the layout's reported document size changes (font / wrap
    // width). Both fire on the same conditions in normal use; we
    // subscribe to both so the gutter is robust against differing
    // backend implementations.
    connect(td,
            &QTextDocument::contentsChanged,
            this,
            &CanvasLineNumberGutter::onDocumentChanged,
            Qt::UniqueConnection);

    if (auto* layout = td->documentLayout()) {
        connect(layout,
                &QAbstractTextDocumentLayout::documentSizeChanged,
                this,
                &CanvasLineNumberGutter::onDocumentChanged,
                Qt::UniqueConnection);
    }
}

void CanvasLineNumberGutter::onDocumentChanged() {
    update();
}

qreal CanvasLineNumberGutter::scrollY() const {
    return m_scrollY;
}

void CanvasLineNumberGutter::setScrollY(qreal y) {
    if (qFuzzyCompare(m_scrollY, y))
        return;
    m_scrollY = y;
    emit scrollYChanged();
    update();
}

QFont CanvasLineNumberGutter::font() const {
    return m_font;
}

void CanvasLineNumberGutter::setFont(const QFont& f) {
    if (m_font == f)
        return;
    m_font = f;
    emit fontChanged();
    update();
}

QColor CanvasLineNumberGutter::textColor() const {
    return m_textColor;
}

void CanvasLineNumberGutter::setTextColor(const QColor& c) {
    if (m_textColor == c)
        return;
    m_textColor = c;
    emit textColorChanged();
    update();
}

QColor CanvasLineNumberGutter::backgroundColor() const {
    return m_backgroundColor;
}

void CanvasLineNumberGutter::setBackgroundColor(const QColor& c) {
    if (m_backgroundColor == c)
        return;
    m_backgroundColor = c;
    emit backgroundColorChanged();
    update();
}

int CanvasLineNumberGutter::rightPadding() const {
    return m_rightPadding;
}

void CanvasLineNumberGutter::setRightPadding(int p) {
    if (m_rightPadding == p)
        return;
    m_rightPadding = p;
    emit rightPaddingChanged();
    update();
}

void CanvasLineNumberGutter::paint(QPainter* painter) {
    if (!painter)
        return;

    // Optional flat background fill — caller controls via property.
    if (m_backgroundColor.alpha() > 0) {
        painter->fillRect(QRectF(0, 0, width(), height()), m_backgroundColor);
    }

    if (!m_currentDoc)
        return;
    QAbstractTextDocumentLayout* layout = m_currentDoc->documentLayout();
    if (!layout)
        return;

    painter->setFont(m_font);
    painter->setPen(m_textColor);

    const qreal viewTop = m_scrollY;
    const qreal viewBottom = m_scrollY + height();
    const qreal w = width() - m_rightPadding;

    // Walk every block. Stop early when we've passed the visible
    // bottom — blocks are in document-order so positions only grow.
    int lineNumber = 1;
    for (QTextBlock block = m_currentDoc->begin(); block.isValid();
         block = block.next(), ++lineNumber) {
        const QRectF r = layout->blockBoundingRect(block);
        const qreal blockTop = r.y();
        const qreal blockBottom = blockTop + r.height();

        if (blockBottom < viewTop)
            continue;  // above visible
        if (blockTop > viewBottom)
            break;  // below visible

        const qreal y = blockTop - m_scrollY;
        const QString num = QString::number(lineNumber);

        // Right-aligned within the gutter, vertically centred on the
        // block's baseline.
        const QRectF cell(0.0, y, w, r.height());
        painter->drawText(cell, Qt::AlignRight | Qt::AlignVCenter, num);
    }
}
