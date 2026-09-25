// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file edit-canvas-tool.cpp
 * @brief Implementation of Tools::EditCanvasTool.
 * @layer Service (Tool subsystem)
 * @dependencies CanvasService.
 */

#include "edit-canvas-tool.h"

#include "../../services/canvas-service.h"
#include "../../utils/escaped-text-repair.h"
#include "../../utils/logger.h"

#include <QJsonObject>

namespace Tools {

EditCanvasTool::EditCanvasTool(CanvasService& canvasSvc,
                               std::function<QString()> activeConvIdGetter)
    : m_canvasSvc(canvasSvc), m_activeConvIdGetter(std::move(activeConvIdGetter)) {}

QString EditCanvasTool::name() const {
    return QStringLiteral("edit_canvas");
}

QString EditCanvasTool::description() const {
    return QStringLiteral("Replace a canvas file's full content. By default edits the currently "
                          "OPEN canvas; pass 'filename' to edit a specific file — if it is not "
                          "the open one it is opened (from an existing canvas or a workspace "
                          "file) and edited, so you can edit several files in a row without a "
                          "separate open_canvas. A name that exists nowhere is refused (create "
                          "it with write_file or open_canvas first). v1 does NOT support delta "
                          "or line-range edits — pass the COMPLETE new file body. If you need "
                          "to preserve existing parts, call read_canvas first and merge the "
                          "unchanged sections into your new content. Revision auto-increments; "
                          "the panel re-renders live. When language=\"json\" (per the canvas "
                          "metadata): emit STRICT JSON only, no // or /* */ comments.");
}

QList<ToolParameterSchema> EditCanvasTool::parameters() const {
    ToolParameterSchema content;
    content.name = QStringLiteral("new_content");
    content.type = QStringLiteral("string");
    content.description =
        QStringLiteral("Full file content (replaces the previous revision verbatim).");
    content.required = true;

    ToolParameterSchema fname;
    fname.name = QStringLiteral("filename");
    fname.type = QStringLiteral("string");
    fname.description =
        QStringLiteral("Optional. The file to edit. If it is not the open canvas it is "
                       "opened (an existing canvas or a workspace file) and edited. Omit to "
                       "edit the active canvas.");
    fname.required = false;

    return {content, fname};
}

bool EditCanvasTool::runsOnMainThread() const {
    return true;
}

QJsonValue EditCanvasTool::invoke(const QJsonObject& args) {
    if (!args.contains(QStringLiteral("new_content"))) {
        const QString msg =
            args.contains(QStringLiteral("__digest"))
                ? QStringLiteral("new_content is required: you sent a summary/placeholder, "
                                 "not the file body. Resend edit_canvas with the full "
                                 "'new_content'.")
                : QStringLiteral("new_content is required");
        return QJsonObject{{QStringLiteral("error"), msg}};
    }
    QString newContent = args[QStringLiteral("new_content")].toString();
    // Same repair as write_file / open_canvas: a double-escaped payload
    // would replace the canvas with one line of literal "\n" sequences.
    if (Verzeta::looksDoubleEscaped(newContent)) {
        newContent = Verzeta::repairDoubleEscapedText(newContent);
        qCWarning(verzetaTools).noquote()
            << "edit_canvas: repaired double-escaped newlines in new_content";
    }

    QString convId = args.value(QStringLiteral("__caller_conv_id")).toString();
    if (convId.isEmpty() && m_activeConvIdGetter) {
        convId = m_activeConvIdGetter();
    }
    if (convId.isEmpty()) {
        return QJsonObject{{QStringLiteral("error"), QStringLiteral("No active conversation")}};
    }

    // Pre-edit metadata snapshot — the active canvas (may be empty).
    const QVariantMap before = m_canvasSvc.activeCanvasFor(convId);
    const QString activeName = before.value(QStringLiteral("filename")).toString();

    // Optional 'filename': the file the caller means to edit. When it is a
    // DIFFERENT file than the open one, RETARGET — switch to that file
    // (an existing canvas, or an existing workspace file read from disk)
    // and apply the edit in one step, instead of forcing a separate
    // open_canvas. This is what "edit file X" naturally means and lets a
    // multi-file batch edit each file. Only a name that exists NOWHERE is
    // refused (create it with write_file / open_canvas first). A matching
    // or omitted target edits the active canvas.
    const QString target = args.value(QStringLiteral("filename")).toString().trimmed();
    if (!target.isEmpty() && target != activeName) {
        const QString cid = m_canvasSvc.retargetAndEdit(convId, target, newContent);
        if (cid.isEmpty()) {
            return QJsonObject{
                {QStringLiteral("error"),
                 QStringLiteral("Cannot edit '%1': there is no open canvas or workspace "
                                "file with that name. Create it first with write_file, "
                                "or open_canvas with its content.")
                     .arg(target)}};
        }
    } else {
        if (before.isEmpty()) {
            return QJsonObject{
                {QStringLiteral("error"), QStringLiteral("No active canvas in this conversation")}};
        }
        if (!m_canvasSvc.editCanvas(convId, newContent)) {
            return QJsonObject{{QStringLiteral("error"), QStringLiteral("Failed to edit canvas")}};
        }
    }

    // Re-fetch metadata so the response carries the fresh revision +
    // lineCount + byteSize the agent might use for next-turn reasoning.
    const QVariantMap after = m_canvasSvc.activeCanvasFor(convId);

    QJsonObject result;
    result[QStringLiteral("canvas_id")] = after.value(QStringLiteral("id")).toString();
    result[QStringLiteral("revision")] = after.value(QStringLiteral("revision")).toInt();
    result[QStringLiteral("lines")] = after.value(QStringLiteral("lineCount")).toInt();
    result[QStringLiteral("byte_size")] = after.value(QStringLiteral("byteSize")).toInt();
    result[QStringLiteral("filename")] = after.value(QStringLiteral("filename")).toString();
    return result;
}

}  // namespace Tools
