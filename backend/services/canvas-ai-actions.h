// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file canvas-ai-actions.h
 * @brief Dynamic AI action bar catalog. Each canvas language family
 *        (code / prose / data) has a tight set of LLM-driven
 *        transformations the user can fire from the action bar.
 *        Every action lives in a single C++ static catalog of
 *        `CanvasAiAction` descriptors; QML reads the per-language
 *        slice via `availableForLanguage(lang)` and renders Primary
 *        actions as bar buttons + Overflow actions inside a `…` menu.
 *
 *        `trigger(actionId, submenuChoice)` composes a deterministic
 *        user-message text from the descriptor's `promptTemplate`
 *        (substituting filename / language / submenu choice) and
 *        calls `ChatController::sendMessage()`. The agent then uses
 *        the existing `read_canvas` / `edit_canvas` / `open_canvas`
 *        tools to do the actual transformation; no new chat tools
 *        are introduced.
 *
 * @layer Service
 * @dependencies CanvasService (active canvas filename + language),
 *               ChatController (sendMessage dispatch).
 */
#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

class CanvasService;
class ChatController;

/**
 * @brief Catalog + dispatch for AI-driven canvas transformations.
 */
class CanvasAiActions : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Construct the service. Dependencies are attached via
     *        setters during AppController initialize.
     * @param parent Qt parent.
     */
    explicit CanvasAiActions(QObject* parent = nullptr);

    /**
     * @brief Attach the canvas service for active-canvas filename +
     *        language lookup.
     * @param svc Non-owning pointer.
     */
    void setCanvasService(CanvasService* svc);

    /**
     * @brief Attach the chat controller used to dispatch the composed
     *        user message.
     * @param cc Non-owning pointer.
     */
    void setChatController(ChatController* cc);

    /**
     * @brief Test seam: override the conversation-id getter so the
     *        unit test can drive trigger() without a real
     *        ChatController. Default reads
     *        ChatController::activeConversationId().
     * @param getter Callable returning the active conversation id.
     */
    void setActiveConversationIdGetter(std::function<QString()> getter);

    /**
     * @brief Action descriptors applicable to a language. Each entry:
     *        `{ id, label, iconName, primary, submenu, description }`.
     * @param language Canvas language tag, e.g. "python", "markdown".
     * @returns List of descriptor maps; empty when no actions apply.
     */
    Q_INVOKABLE QVariantList availableForLanguage(const QString& language) const;

    /**
     * @brief Compose the prompt for `actionId` (and submenu choice
     *        when the descriptor has a submenu) and call
     *        ChatController::sendMessage. Emits errorOccurred on
     *        missing canvas / unknown action / no chat controller
     *        attached.
     * @param actionId      Descriptor id, e.g. "code.add-comments".
     * @param submenuChoice When the descriptor carries a submenu
     *                      list, the chosen value, such as "python";
     *                      empty for non-submenu actions.
     * @returns true on successful dispatch; false on validation
     *          failure.
     */
    Q_INVOKABLE bool trigger(const QString& actionId, const QString& submenuChoice = {});

  signals:
    /**
     * @brief Emitted when trigger() fails to dispatch a prompt.
     * @param message Human-readable failure description.
     */
    void errorOccurred(const QString& message);

  private:
    CanvasService* m_canvasSvc = nullptr;
    ChatController* m_chat = nullptr;
    std::function<QString()> m_activeConvIdGetter;

    /**
     * @brief Internal action descriptor: the static catalog row,
     *        returned by availableForLanguage filtered on family.
     */
    struct Action {
        QString id;
        QString label;
        QString iconName;
        QString description;
        QString promptTemplate;    // %1=filename %2=language %3=submenu choice
        QStringList families;      // {"code","prose","data"}
        QStringList languageGate;  // empty = any in family; otherwise specific tags
        QStringList languageDeny;  // hide for these tags (e.g. "json" for Add Comments)
        bool primary = false;
        QStringList submenu;  // non-empty when the action expands into a choice menu
    };

    /** @brief The static catalog of all action descriptors. */
    static const QList<Action>& catalog();

    /** @brief Map a language tag to a family. Returns empty for unknown. */
    static QString familyFor(const QString& language);
};
