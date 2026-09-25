// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file canvas-ai-actions.cpp
 * @brief Implementation of CanvasAiActions. See the header.
 * @layer Service
 * @dependencies CanvasService, ChatController, Qt6::Core.
 */

#include "canvas-ai-actions.h"

#include "../services/canvas-service.h"
#include "../services/chat-controller.h"
#include "../utils/logger.h"
#include "../utils/thread-discipline.h"

#include <QFileInfo>
#include <QSet>

namespace {

// Submenu lists shared across actions.
const QStringList kPortToLanguages = {
    QStringLiteral("python"),
    QStringLiteral("javascript"),
    QStringLiteral("typescript"),
    QStringLiteral("go"),
    QStringLiteral("rust"),
    QStringLiteral("c"),
    QStringLiteral("cpp"),
    QStringLiteral("java"),
    QStringLiteral("shell"),
    QStringLiteral("html"),
};
const QStringList kConvertDataLanguages = {
    QStringLiteral("json"),
    QStringLiteral("yaml"),
    QStringLiteral("toml"),
};
const QStringList kTranslateLanguages = {
    QStringLiteral("English"),
    QStringLiteral("Spanish"),
    QStringLiteral("French"),
    QStringLiteral("German"),
    QStringLiteral("Chinese"),
    QStringLiteral("Japanese"),
    QStringLiteral("Hindi"),
    QStringLiteral("Arabic"),
};
const QStringList kToneOptions = {
    QStringLiteral("Formal"),
    QStringLiteral("Casual"),
    QStringLiteral("Friendly"),
    QStringLiteral("Professional"),
    QStringLiteral("Concise"),
};

}  // namespace

const QList<CanvasAiActions::Action>& CanvasAiActions::catalog() {
    static const QList<Action> kCatalog = {
        // ===== Code family =====
        Action{QStringLiteral("code.add-comments"),
               QStringLiteral("Add Comments"),
               QStringLiteral("documentinfo"),
               QStringLiteral("Add inline comments explaining each non-trivial block"),
               QStringLiteral("Please add inline comments to the active canvas (filename: %1, "
                              "language: %2). Use read_canvas to read the current content, then "
                              "edit_canvas with the fully-commented version. Keep the original "
                              "logic identical and add comments only."),
               {QStringLiteral("code")},
               {},
               {},
               true,
               {}},
        Action{QStringLiteral("code.fix-bugs"),
               QStringLiteral("Fix Bugs"),
               QStringLiteral("debug-step-into"),
               QStringLiteral("Find and fix logic, null, off-by-one and security bugs"),
               QStringLiteral("Please review the active canvas (%1, %2) for bugs (logic errors, "
                              "off-by-ones, null derefs, race conditions, security issues) and "
                              "fix them. read_canvas first, then edit_canvas with the fixed "
                              "version. Describe what you changed in a short reply after the "
                              "tool call."),
               {QStringLiteral("code")},
               {},
               {},
               true,
               {}},
        Action{
            QStringLiteral("code.refactor"),
            QStringLiteral("Refactor"),
            QStringLiteral("edit-cut"),
            QStringLiteral("Improve structure, naming and readability without changing behaviour"),
            QStringLiteral("Please refactor the active canvas (%1, %2): improve naming, "
                           "structure and readability, and eliminate duplication. Use read_canvas, "
                           "then edit_canvas. Behaviour MUST stay identical; describe the "
                           "key shape changes in a short reply."),
            {QStringLiteral("code")},
            {},
            {},
            true,
            {}},
        Action{QStringLiteral("code.port"),
               QStringLiteral("Port to Language…"),
               QStringLiteral("preferences-desktop-locale"),
               QStringLiteral("Translate this code into another language"),
               QStringLiteral("Please port the active canvas (%1, %2) to %3. read_canvas first, "
                              "then call open_canvas with a basename matching the original plus "
                              "the new language's standard extension, language \"%3\", and the "
                              "ported content. Keep behaviour identical; comment any non-obvious "
                              "mapping."),
               {QStringLiteral("code")},
               {},
               {},
               true,
               kPortToLanguages},
        // Overflow code actions
        Action{QStringLiteral("code.add-docstrings"),
               QStringLiteral("Add Docstrings"),
               QStringLiteral("documentation"),
               QStringLiteral("Function-level documentation in the language's idiom"),
               QStringLiteral("Please add idiomatic docstrings / docblocks to every function and "
                              "class in the active canvas (%1, %2). read_canvas, edit_canvas. "
                              "Do not modify executable code; add documentation only."),
               {QStringLiteral("code")},
               {},
               {},
               false,
               {}},
        Action{QStringLiteral("code.add-logs"),
               QStringLiteral("Add Logs"),
               QStringLiteral("emblem-system"),
               QStringLiteral("Add idiomatic logging at function entry, branches and errors"),
               QStringLiteral("Please add idiomatic logging statements to the active canvas "
                              "(%1, %2) at every function entry, every branch, and every error "
                              "path. read_canvas, edit_canvas. Do not change behaviour; "
                              "add logging only."),
               {QStringLiteral("code")},
               {},
               {},
               false,
               {}},
        Action{QStringLiteral("code.generate-tests"),
               QStringLiteral("Generate Tests"),
               QStringLiteral("test"),
               QStringLiteral("Create unit tests for the canvas content"),
               QStringLiteral("Please generate idiomatic unit tests for the active canvas "
                              "(%1, %2). read_canvas first. Then call open_canvas with a name "
                              "like \"<basename>_test.<ext>\" carrying the test content. Keep "
                              "tests focused; cover happy path + 1-2 edge cases."),
               {QStringLiteral("code")},
               {},
               {},
               false,
               {}},
        Action{QStringLiteral("code.explain"),
               QStringLiteral("Explain"),
               QStringLiteral("help-hint"),
               QStringLiteral("Walk through what the code does"),
               QStringLiteral("Please explain the active canvas (%1, %2). read_canvas first, "
                              "then reply with a clear, structured explanation: high-level "
                              "purpose, key abstractions, control flow, anything subtle. Do "
                              "NOT call edit_canvas; reply with text only."),
               {QStringLiteral("code")},
               {},
               {},
               false,
               {}},
        Action{QStringLiteral("code.optimize"),
               QStringLiteral("Optimize"),
               QStringLiteral("speedometer"),
               QStringLiteral("Improve performance and readability"),
               QStringLiteral("Please optimize the active canvas (%1, %2) for performance and "
                              "readability. read_canvas, edit_canvas. Describe each material "
                              "change briefly in your reply."),
               {QStringLiteral("code")},
               {},
               {},
               false,
               {}},
        Action{
            QStringLiteral("code.add-types"),
            QStringLiteral("Add Type Annotations"),
            QStringLiteral("text-x-script"),
            QStringLiteral(
                "Add type hints, such as Python type hints, or convert JavaScript to TypeScript"),
            QStringLiteral("Please add or strengthen type annotations in the active canvas "
                           "(%1, %2). For Python use PEP 484 hints; for JavaScript convert "
                           "to TypeScript via open_canvas with a .ts extension. read_canvas, "
                           "then edit_canvas (or open_canvas for JS→TS)."),
            {QStringLiteral("code")},
            {},
            {},
            false,
            {}},

        // ===== Prose family =====
        Action{QStringLiteral("prose.autocomplete"),
               QStringLiteral("Autocomplete"),
               QStringLiteral("text-completion"),
               QStringLiteral("Continue writing from where the canvas leaves off"),
               QStringLiteral("Continue writing the active canvas (%1) from where it leaves off. "
                              "read_canvas to see the current content, then edit_canvas with "
                              "the original text PLUS your continuation appended. Match tone, "
                              "voice, and formatting. Don't restate or rewrite the prior text; "
                              "only extend it."),
               {QStringLiteral("prose")},
               {},
               {},
               true,
               {}},
        Action{QStringLiteral("prose.grammify"),
               QStringLiteral("Fix Grammar"),
               QStringLiteral("tools-check-spelling"),
               QStringLiteral("Polish grammar, spelling, structure and tone"),
               QStringLiteral("Improve the active canvas (%1) for grammar, spelling, sentence "
                              "structure, formatting, and professional tone, while preserving "
                              "the author's meaning and voice. read_canvas first; edit_canvas "
                              "with the polished version. List the categories of changes "
                              "(grammar / spelling / structure / etc.) in your reply but do "
                              "not quote individual edits."),
               {QStringLiteral("prose")},
               {},
               {},
               true,
               {}},
        Action{QStringLiteral("prose.summarize"),
               QStringLiteral("Summarize"),
               QStringLiteral("view-list-text"),
               QStringLiteral("Condense to a short summary"),
               QStringLiteral("Summarize the active canvas (%1) into a short, focused summary. "
                              "read_canvas, then edit_canvas with ONLY the summary as content. "
                              "The user can undo this edit. To keep the original visible as well, "
                              "you may ALSO call open_canvas with a \"<basename>-summary.md\" "
                              "name carrying the summary. Choose "
                              "based on whether the canvas appears to be a draft or a source."),
               {QStringLiteral("prose")},
               {},
               {},
               true,
               {}},
        Action{QStringLiteral("prose.spin"),
               QStringLiteral("Paraphrase"),
               QStringLiteral("view-refresh"),
               QStringLiteral("Paraphrase while keeping the meaning"),
               QStringLiteral(
                   "Paraphrase the active canvas (%1). Preserve every fact and the "
                   "overall meaning, but rewrite the wording so the result reads as "
                   "a fresh draft. read_canvas first, then edit_canvas with the paraphrased "
                   "version. Match the original length within 10%."),
               {QStringLiteral("prose")},
               {},
               {},
               true,
               {}},
        // Overflow prose
        Action{QStringLiteral("prose.expand"),
               QStringLiteral("Expand"),
               QStringLiteral("zoom-in"),
               QStringLiteral("Add depth, examples and detail"),
               QStringLiteral("Expand the active canvas (%1): add depth, examples and supporting "
                              "detail. Preserve the original argument and structure. "
                              "read_canvas, edit_canvas. Aim for ~1.5×–2× the original length."),
               {QStringLiteral("prose")},
               {},
               {},
               false,
               {}},
        Action{QStringLiteral("prose.outline"),
               QStringLiteral("Outline"),
               QStringLiteral("view-list-tree"),
               QStringLiteral("Extract a hierarchical outline"),
               QStringLiteral("Generate a hierarchical outline of the active canvas (%1). "
                              "read_canvas first, then call open_canvas with name "
                              "\"<basename>-outline.md\", language \"markdown\", and the "
                              "outline as nested markdown lists. Do not modify the original."),
               {QStringLiteral("prose")},
               {},
               {},
               false,
               {}},
        Action{QStringLiteral("prose.title"),
               QStringLiteral("Generate Title"),
               QStringLiteral("draw-text"),
               QStringLiteral("Suggest 3-5 candidate titles"),
               QStringLiteral("Read the active canvas (%1) and suggest 3-5 candidate titles in "
                              "your reply, one per line. Do NOT call edit_canvas."),
               {QStringLiteral("prose")},
               {},
               {},
               false,
               {}},
        Action{QStringLiteral("prose.tone"),
               QStringLiteral("Change Tone…"),
               QStringLiteral("face-smile"),
               QStringLiteral("Rewrite with a different tone"),
               QStringLiteral("Rewrite the active canvas (%1) in a more %3 tone. Preserve "
                              "every fact and the overall structure. read_canvas, edit_canvas."),
               {QStringLiteral("prose")},
               {},
               {},
               false,
               kToneOptions},
        Action{QStringLiteral("prose.translate"),
               QStringLiteral("Translate…"),
               QStringLiteral("preferences-desktop-locale"),
               QStringLiteral("Translate to another language"),
               QStringLiteral("Translate the active canvas (%1) to %3. Preserve formatting "
                              "(markdown headers, lists, code fences). read_canvas first, "
                              "edit_canvas with the translated content."),
               {QStringLiteral("prose")},
               {},
               {},
               false,
               kTranslateLanguages},
        Action{QStringLiteral("prose.convert-md-html"),
               QStringLiteral("Convert (Markdown ↔ HTML)"),
               QStringLiteral("text-html"),
               QStringLiteral("Convert Markdown to HTML, or HTML to Markdown"),
               QStringLiteral(
                   "Convert the active canvas (%1, language %2) between Markdown "
                   "and HTML, converting to whichever format the source is not. read_canvas first, "
                   "then call open_canvas with the converted content using a sensible "
                   "filename + language."),
               {QStringLiteral("prose")},
               {},
               {},
               false,
               {}},

        // ===== Data family =====
        Action{QStringLiteral("data.convert"),
               QStringLiteral("Convert…"),
               QStringLiteral("preferences-desktop-locale"),
               QStringLiteral("Convert between JSON, YAML and TOML"),
               QStringLiteral("Convert the active canvas (%1, %2) to %3. read_canvas first, "
                              "then call open_canvas with a basename matching the original "
                              "plus the .%3 extension, language \"%3\", and the converted "
                              "content. Preserve every field and value verbatim."),
               {QStringLiteral("data")},
               {},
               {},
               true,
               kConvertDataLanguages},
        Action{QStringLiteral("data.generate-schema"),
               QStringLiteral("Generate Schema"),
               QStringLiteral("text-x-generic-template"),
               QStringLiteral("Create a JSON Schema describing the data shape"),
               QStringLiteral("Read the active canvas (%1, %2) and generate a JSON Schema "
                              "(Draft 2020-12) describing its structure. Call open_canvas "
                              "with name \"<basename>.schema.json\", language \"json\", and "
                              "the schema as content."),
               {QStringLiteral("data")},
               {},
               {},
               true,
               {}},
        Action{QStringLiteral("data.annotate"),
               QStringLiteral("Annotate"),
               QStringLiteral("documentinfo"),
               QStringLiteral("Add inline help-text comments to each field"),
               QStringLiteral("Read the active canvas (%1, %2) and add inline comments above "
                              "every non-trivial field explaining what it controls and what "
                              "valid values are. edit_canvas with the annotated version."),
               // JSON doesn't support comments — hide there.
               {QStringLiteral("data")},
               {},
               {QStringLiteral("json")},
               true,
               {}},
        Action{QStringLiteral("data.from-description"),
               QStringLiteral("Generate from Description"),
               QStringLiteral("document-new"),
               QStringLiteral("Ask the user for a description, then generate the data"),
               QStringLiteral("The active canvas (%1, %2) is currently a placeholder. Ask "
                              "the user a focused 1-2 sentence question about what data "
                              "they want, then on their reply call edit_canvas with a "
                              "complete %2 document matching their description."),
               {QStringLiteral("data")},
               {},
               {},
               true,
               {}},
        // Overflow data
        Action{QStringLiteral("data.add-comments"),
               QStringLiteral("Add Comments"),
               QStringLiteral("documentinfo"),
               QStringLiteral("Add inline comments above each section"),
               QStringLiteral("Add inline comments above each non-trivial section of the "
                              "active canvas (%1, %2). read_canvas, edit_canvas. Comments "
                              "only. Do not modify field values."),
               // JSON doesn't support comments.
               {QStringLiteral("data")},
               {},
               {QStringLiteral("json")},
               false,
               {}},
        Action{QStringLiteral("data.minify"),
               QStringLiteral("Minify"),
               QStringLiteral("zoom-out"),
               QStringLiteral("Strip all whitespace and comments"),
               QStringLiteral("Minify the active canvas (%1, %2): strip every non-essential "
                              "whitespace and comment, and keep semantics identical. read_canvas, "
                              "edit_canvas."),
               {QStringLiteral("data")},
               {},
               {},
               false,
               {}},
        Action{QStringLiteral("data.sort-keys"),
               QStringLiteral("Sort Keys"),
               QStringLiteral("view-sort-ascending"),
               QStringLiteral("Sort object keys alphabetically"),
               QStringLiteral("Sort all object keys in the active canvas (%1, %2) "
                              "alphabetically (recursively for nested objects). Preserve "
                              "every value verbatim. read_canvas, edit_canvas."),
               {QStringLiteral("data")},
               {},
               {},
               false,
               {}},
        Action{QStringLiteral("data.validate-against"),
               QStringLiteral("Validate Against Schema"),
               QStringLiteral("dialog-ok-apply"),
               QStringLiteral("Ask for a schema URL, validate, and report violations"),
               QStringLiteral("Ask the user for a JSON Schema URL or pasted schema, then "
                              "read the active canvas (%1, %2), validate it, and report "
                              "any violations in your reply. Do NOT call edit_canvas."),
               {QStringLiteral("data")},
               {},
               {},
               false,
               {}},
    };
    return kCatalog;
}

QString CanvasAiActions::familyFor(const QString& language) {
    static const QSet<QString> kCode = {
        QStringLiteral("python"),
        QStringLiteral("shell"),
        QStringLiteral("bash"),
        QStringLiteral("cpp"),
        QStringLiteral("c"),
        QStringLiteral("h"),
        QStringLiteral("hpp"),
        QStringLiteral("javascript"),
        QStringLiteral("typescript"),
        QStringLiteral("java"),
        QStringLiteral("go"),
        QStringLiteral("rust"),
        QStringLiteral("html"),
        QStringLiteral("css"),
        QStringLiteral("qml"),
    };
    static const QSet<QString> kProse = {
        QStringLiteral("markdown"),
        QStringLiteral("plaintext"),
    };
    static const QSet<QString> kData = {
        QStringLiteral("json"),
        QStringLiteral("yaml"),
        QStringLiteral("toml"),
        QStringLiteral("ini"),
    };
    if (kCode.contains(language))
        return QStringLiteral("code");
    if (kProse.contains(language))
        return QStringLiteral("prose");
    if (kData.contains(language))
        return QStringLiteral("data");
    return {};
}

CanvasAiActions::CanvasAiActions(QObject* parent) : QObject(parent) {}

void CanvasAiActions::setCanvasService(CanvasService* svc) {
    m_canvasSvc = svc;
}
void CanvasAiActions::setChatController(ChatController* cc) {
    m_chat = cc;
}
void CanvasAiActions::setActiveConversationIdGetter(std::function<QString()> getter) {
    m_activeConvIdGetter = std::move(getter);
}

QVariantList CanvasAiActions::availableForLanguage(const QString& language) const {
    VERZETA_ASSERT_MAIN_THREAD();
    const QString family = familyFor(language);
    QVariantList out;
    if (family.isEmpty())
        return out;

    for (const Action& a : catalog()) {
        if (!a.families.contains(family))
            continue;
        if (!a.languageGate.isEmpty() && !a.languageGate.contains(language))
            continue;
        if (a.languageDeny.contains(language))
            continue;
        QVariantMap m;
        m[QStringLiteral("id")] = a.id;
        m[QStringLiteral("label")] = a.label;
        m[QStringLiteral("iconName")] = a.iconName;
        m[QStringLiteral("description")] = a.description;
        m[QStringLiteral("primary")] = a.primary;
        m[QStringLiteral("submenu")] = a.submenu;
        out.append(m);
    }
    return out;
}

bool CanvasAiActions::trigger(const QString& actionId, const QString& submenuChoice) {
    VERZETA_ASSERT_MAIN_THREAD();
    if (actionId.isEmpty()) {
        emit errorOccurred(QStringLiteral("Empty action id"));
        return false;
    }
    if (!m_canvasSvc) {
        emit errorOccurred(QStringLiteral("Canvas service not attached"));
        return false;
    }

    const QString convId = m_activeConvIdGetter
                               ? m_activeConvIdGetter()
                               : (m_chat ? m_chat->activeConversationId() : QString());
    if (convId.isEmpty()) {
        emit errorOccurred(QStringLiteral("No active conversation"));
        return false;
    }
    const QVariantMap active = m_canvasSvc->activeCanvasFor(convId);
    if (active.isEmpty()) {
        emit errorOccurred(QStringLiteral("No active canvas"));
        return false;
    }

    const Action* match = nullptr;
    for (const Action& a : catalog()) {
        if (a.id == actionId) {
            match = &a;
            break;
        }
    }
    if (!match) {
        emit errorOccurred(QStringLiteral("Unknown action: %1").arg(actionId));
        return false;
    }

    const QString filename = active.value(QStringLiteral("filename")).toString();
    const QString language = active.value(QStringLiteral("language")).toString();

    // %1 = filename, %2 = language, %3 = submenu choice (or empty).
    const QString prompt = match->promptTemplate.arg(filename, language, submenuChoice);

    if (!m_chat) {
        // Test mode — getter set, no chat. Treat success as "would
        // dispatch" so the test can assert via the captured prompt
        // through a side-channel.
        qCInfo(verzetaUi) << "CanvasAiActions::trigger (no chat):" << actionId << submenuChoice;
        return true;
    }
    m_chat->sendMessage(prompt);
    qCInfo(verzetaUi) << "CanvasAiActions::trigger:" << actionId << submenuChoice;
    return true;
}
