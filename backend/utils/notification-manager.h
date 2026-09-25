// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file notification-manager.h
 * @brief Sends desktop notifications via KNotifications (KF6).
 *        Uses the FreeDesktop org.freedesktop.Notifications DBus spec under
 *        the hood, so notifications surface correctly on KDE Plasma 5/6,
 *        GNOME, Cinnamon, and any other desktop with a notification daemon.
 * @layer Utility
 * @dependencies KF6::Notifications, Qt6::Core, Qt6::Gui
 */


#pragma once

#include <QObject>
#include <QString>

/**
 * @brief Singleton utility for sending desktop notifications via
 *        KNotifications.
 *
 * ## Usage
 *
 *   NotificationManager::instance().notifyResponseComplete("My Chat", "Sure…");
 *   NotificationManager::instance().notifyMediaReady("image", "My Chat");
 *   NotificationManager::instance().notifyUserMentioned("Group A", "@Alice", "ping");
 *
 * ## Lifetime
 *
 * `instance()` is a Meyers singleton. Construction is cheap (no
 * resources allocated; KNotification objects are created per-event
 * and self-delete after dispatch). `shutdown()` is retained as a
 * no-op for source-compat with the previous QSystemTrayIcon-era
 * code that called it from QCoreApplication::aboutToQuit; future
 * calls can drop it entirely.
 *
 * ## Event types (must match resources/verzeta-studio.notifyrc)
 *
 *   "responseComplete":   LLM response ready in a background conversation
 *   "userMentioned":      agent @-mentioned the user in a group chat
 *   "imageReady":         image generation completed
 *   "audioReady":         audio generation completed
 *   "info":               generic informational fallback
 */
class NotificationManager : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Returns the singleton instance.
     * @return Reference to the NotificationManager singleton.
     */
    static NotificationManager& instance();

    /**
     * @brief Sends a generic-info notification.
     *
     * Body is truncated at 200 characters to avoid overwhelming the
     * notification popup or accidentally surfacing long sensitive content.
     *
     * @param title    Notification title (e.g., application name).
     * @param body     Notification body text.
     * @param iconName FreeDesktop icon name (resolved against the host icon
     *                 theme; falls back to the bundled application icon).
     */
    void notify(const QString& title,
                const QString& body,
                const QString& iconName = QStringLiteral("verzeta-studio"));

    /**
     * @brief Notification: an LLM response completed in the background.
     * @param conversationTitle Title of the conversation that received the response.
     * @param previewText       First ~100 chars of the assistant response.
     */
    void notifyResponseComplete(const QString& conversationTitle, const QString& previewText);

    /**
     * @brief Notification: image or audio generation completed.
     * @param type              "image" or "audio"; selects the .notifyrc event.
     * @param conversationTitle Title of the conversation.
     */
    void notifyMediaReady(const QString& type, const QString& conversationTitle);

    /**
     * @brief Notification: an agent @-mentioned the user in a group chat.
     * @param conversationTitle Title of the group chat.
     * @param agentAlias        @-handle of the mentioning agent.
     * @param messageBody       Short preview of the mentioning message.
     */
    void notifyUserMentioned(const QString& conversationTitle,
                             const QString& agentAlias,
                             const QString& messageBody);

    /**
     * @brief No-op kept for source-compat with the old QSystemTrayIcon
     *        code path that needed teardown coordination with QApplication.
     *        KNotification has no resources to release.
     */
    void shutdown();

  private:
    /** @brief Private constructor; use instance(). */
    NotificationManager();

    /** @brief Default destructor; no resources held. */
    ~NotificationManager() override;

    /**
     * @brief Internal helper: dispatch a KNotification event.
     * @param eventId   Event id from verzeta-studio.notifyrc (e.g. "responseComplete").
     * @param title     Localised notification title.
     * @param body      Localised notification body, truncated to 200 chars.
     * @param iconName  Override icon name; pass empty to use the event's
     *                  default from the .notifyrc.
     */
    void dispatchEvent(const QString& eventId,
                       const QString& title,
                       const QString& body,
                       const QString& iconName = QString());
};
