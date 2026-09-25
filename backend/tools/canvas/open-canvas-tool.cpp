// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file open-canvas-tool.cpp
 * @brief Implementation of Tools::OpenCanvasTool.
 * @layer Service (Tool subsystem)
 * @dependencies CanvasService (the actual DB + disk-mirror write).
 */

#include "open-canvas-tool.h"

#include "../../services/canvas-service.h"
#include "../../utils/escaped-text-repair.h"
#include "../../utils/logger.h"

#include <QJsonObject>

namespace Tools {

OpenCanvasTool::OpenCanvasTool(CanvasService& canvasSvc,
                               std::function<QString()> activeConvIdGetter)
    : m_canvasSvc(canvasSvc), m_activeConvIdGetter(std::move(activeConvIdGetter)) {}

QString OpenCanvasTool::name() const {
    return QStringLiteral("open_canvas");
}

QString OpenCanvasTool::description() const {
    return QStringLiteral("Open a file in the Canvas panel for editing. To OPEN AN EXISTING "
                          "workspace file, pass just its 'filename' and omit 'content' — the "
                          "file is read from the project workspace so you can then edit_canvas "
                          "it. To CREATE a new file, pass 'filename', 'language' and the full "
                          "'content'. Use canvas when the user asks for a file (JSON, code, "
                          "config, long markdown) that is at least 500 chars OR that they will "
                          "want to edit, and when they say 'show/open in canvas' regardless of "
                          "length. Open canvas INSTEAD OF dumping the content inline — your "
                          "reply text becomes a short note. For language=\"json\": emit STRICT "
                          "JSON only, no // or /* */ comments.");
}

QList<ToolParameterSchema> OpenCanvasTool::parameters() const {
    ToolParameterSchema fname;
    fname.name = QStringLiteral("filename");
    fname.type = QStringLiteral("string");
    fname.description =
        QStringLiteral("Display filename including extension (e.g. \"config.json\", "
                       "\"main.py\"). Same filename in the same conversation upserts "
                       "an existing canvas with revision++; a different filename "
                       "archives the prior canvas and creates a new active one.");
    fname.required = true;

    ToolParameterSchema lang;
    lang.name = QStringLiteral("language");
    lang.type = QStringLiteral("string");
    lang.description =
        QStringLiteral("Syntax-highlight language tag (required when creating with "
                       "'content'; inferred from the extension when opening an existing "
                       "file). Common values: \"json\", \"python\", \"javascript\", "
                       "\"typescript\", \"cpp\", \"shell\", \"markdown\", \"yaml\", "
                       "\"plaintext\".");
    lang.required = false;

    ToolParameterSchema content;
    content.name = QStringLiteral("content");
    content.type = QStringLiteral("string");
    content.description =
        QStringLiteral("Full file content when CREATING a file. Omit to OPEN an existing "
                       "workspace file of that name (read from disk). The canvas always "
                       "stores the whole file; for edits, call edit_canvas with the new "
                       "full content.");
    content.required = false;

    ToolParameterSchema src;
    src.name = QStringLiteral("source_msg_id");
    src.type = QStringLiteral("string");
    src.description = QStringLiteral("Optional — message id that produced this canvas. Used for "
                                     "auditing; agents can omit.");
    src.required = false;

    return {fname, lang, content, src};
}

bool OpenCanvasTool::runsOnMainThread() const {
    return true;
}

QJsonValue OpenCanvasTool::invoke(const QJsonObject& args) {
    const QString filename = args[QStringLiteral("filename")].toString().trimmed();
    const QString language = args[QStringLiteral("language")].toString().trimmed();
    QString content = args[QStringLiteral("content")].toString();
    const QString sourceMsgId = args[QStringLiteral("source_msg_id")].toString();

    // Same repair as write_file: a double-escaped content payload would
    // otherwise render the canvas as one line of literal "\n" sequences.
    if (Verzeta::looksDoubleEscaped(content)) {
        content = Verzeta::repairDoubleEscapedText(content);
        qCWarning(verzetaTools).noquote()
            << "open_canvas: repaired double-escaped newlines in content for" << filename;
    }

    if (filename.isEmpty()) {
        return QJsonObject{{QStringLiteral("error"), QStringLiteral("filename is required")}};
    }

    QString convId = args.value(QStringLiteral("__caller_conv_id")).toString();
    if (convId.isEmpty() && m_activeConvIdGetter) {
        convId = m_activeConvIdGetter();
    }
    if (convId.isEmpty()) {
        return QJsonObject{{QStringLiteral("error"), QStringLiteral("No active conversation")}};
    }

    if (content.isEmpty()) {
        if (args.contains(QStringLiteral("__digest"))) {
            return QJsonObject{
                {QStringLiteral("error"),
                 QStringLiteral("content is required: you sent a summary/placeholder, "
                                "not the file body. Resend open_canvas with the full "
                                "'content'.")}};
        }
        // OPEN-EXISTING: no content supplied → read the named file from
        // the project workspace so the agent can edit_canvas it. This is
        // the "open the correct file first, then edit" flow; a NULL-content
        // insert previously failed here with a database constraint error.
        const QString canvasId = m_canvasSvc.openCanvasFromWorkspace(convId, filename);
        if (canvasId.isEmpty()) {
            return QJsonObject{
                {QStringLiteral("error"),
                 QStringLiteral("No file named '%1' found in this workspace to open. To "
                                "open an existing file pass its exact name (read_file it "
                                "first if unsure it exists locally); to create a new "
                                "file, call open_canvas with the full 'content'.")
                     .arg(filename)}};
        }
        return QJsonObject{{QStringLiteral("canvas_id"), canvasId},
                           {QStringLiteral("filename"), filename},
                           {QStringLiteral("opened"), QStringLiteral("existing")}};
    }

    // CREATE: content supplied → a language tag is required.
    if (language.isEmpty()) {
        return QJsonObject{
            {QStringLiteral("error"),
             QStringLiteral("language is required when creating a canvas with 'content' "
                            "(e.g. \"markdown\", \"python\", \"json\").")}};
    }

    const QString canvasId =
        m_canvasSvc.openCanvas(convId, filename, language, content, sourceMsgId);
    if (canvasId.isEmpty()) {
        return QJsonObject{
            {QStringLiteral("error"), QStringLiteral("Failed to open canvas: %1").arg(filename)}};
    }

    QJsonObject result;
    result[QStringLiteral("canvas_id")] = canvasId;
    result[QStringLiteral("filename")] = filename;
    result[QStringLiteral("language")] = language;
    result[QStringLiteral("lines")] = content.count(QLatin1Char('\n')) + 1;
    return result;
}

}  // namespace Tools
