// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file read-canvas-tool.cpp
 * @brief Implementation of Tools::ReadCanvasTool.
 * @layer Service (Tool subsystem)
 * @dependencies CanvasService.
 */

#include "read-canvas-tool.h"

#include "../../services/canvas-service.h"

#include <QJsonObject>

namespace Tools {

ReadCanvasTool::ReadCanvasTool(CanvasService& canvasSvc,
                               std::function<QString()> activeConvIdGetter)
    : m_canvasSvc(canvasSvc), m_activeConvIdGetter(std::move(activeConvIdGetter)) {}

QString ReadCanvasTool::name() const {
    return QStringLiteral("read_canvas");
}

QString ReadCanvasTool::description() const {
    return QStringLiteral("Read a 1-based line range from the active canvas in this "
                          "conversation. The system prompt only carries canvas METADATA "
                          "(filename / language / line count / revision); CALL THIS TOOL "
                          "to fetch any content you need to read or modify. Always "
                          "read_canvas BEFORE edit_canvas if you need to preserve any "
                          "existing parts of the file.");
}

QList<ToolParameterSchema> ReadCanvasTool::parameters() const {
    ToolParameterSchema startLine;
    startLine.name = QStringLiteral("start_line");
    startLine.type = QStringLiteral("integer");
    startLine.description = QStringLiteral("First line to include (1-based). Default: 1.");
    startLine.required = false;

    ToolParameterSchema endLine;
    endLine.name = QStringLiteral("end_line");
    endLine.type = QStringLiteral("integer");
    endLine.description =
        QStringLiteral("Last line to include (1-based). Default: -1 = end of file.");
    endLine.required = false;

    return {startLine, endLine};
}

bool ReadCanvasTool::runsOnMainThread() const {
    return true;
}

QJsonValue ReadCanvasTool::invoke(const QJsonObject& args) {
    QString convId = args.value(QStringLiteral("__caller_conv_id")).toString();
    if (convId.isEmpty() && m_activeConvIdGetter) {
        convId = m_activeConvIdGetter();
    }
    if (convId.isEmpty()) {
        return QJsonObject{{QStringLiteral("error"), QStringLiteral("No active conversation")}};
    }

    // Confirm a canvas is active so we can return a precise error
    // distinct from an empty range.
    const QVariantMap active = m_canvasSvc.activeCanvasFor(convId);
    if (active.isEmpty()) {
        return QJsonObject{
            {QStringLiteral("error"), QStringLiteral("No active canvas in this conversation")}};
    }
    const int totalLines = active.value(QStringLiteral("lineCount")).toInt();

    int startLine = 1;
    int endLine = -1;
    if (args.contains(QStringLiteral("start_line"))) {
        startLine = args[QStringLiteral("start_line")].toInt(1);
    }
    if (args.contains(QStringLiteral("end_line"))) {
        endLine = args[QStringLiteral("end_line")].toInt(-1);
    }

    const QString slice = m_canvasSvc.readCanvasSlice(convId, startLine, endLine);
    if (slice.isEmpty() && totalLines > 0 &&
        (startLine > totalLines || (endLine != -1 && endLine < startLine))) {
        return QJsonObject{
            {QStringLiteral("error"), QStringLiteral("Invalid line range")},
            {QStringLiteral("total_lines"), totalLines},
        };
    }

    QJsonObject result;
    result[QStringLiteral("start_line")] = startLine;
    result[QStringLiteral("end_line")] = (endLine == -1) ? totalLines : endLine;
    result[QStringLiteral("content")] = slice;
    result[QStringLiteral("total_lines")] = totalLines;
    return result;
}

}  // namespace Tools
