// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file notification-manager.cpp
 * @brief Implementation of the KNotification-based notification dispatcher.
 *        See header for design rationale; this file just wires the public
 *        API into KNotification::event() calls with the event ids defined
 *        in resources/verzeta-studio.notifyrc.
 * @layer Utility
 * @dependencies KF6::Notifications
 */

#include "notification-manager.h"

#include <KNotification>
#include <QGuiApplication>
#include <QString>

// ---------------------------------------------------------------------------
// Singleton
// ---------------------------------------------------------------------------

NotificationManager& NotificationManager::instance() {
    static NotificationManager inst;
    return inst;
}

NotificationManager::NotificationManager() : QObject(nullptr) {}

NotificationManager::~NotificationManager() = default;

void NotificationManager::shutdown() {
    // KNotification is fire-and-forget — each event self-deletes after
    // dispatch via KNotification's internal lifecycle. No tray icon to
    // tear down anymore (the previous QSystemTrayIcon-based impl needed
    // explicit teardown to avoid SEGV at exit-handlers time; KNotification
    // doesn't have that problem).
}

// ---------------------------------------------------------------------------
// Internal dispatch
// ---------------------------------------------------------------------------

void NotificationManager::dispatchEvent(const QString& eventId,
                                        const QString& title,
                                        const QString& body,
                                        const QString& iconName) {
    // KNotification::event() is the static convenience that creates a
    // self-deleting KNotification, fills in title/text/icon, sets the
    // component name to match the .notifyrc filename ("verzeta-studio"),
    // and dispatches via DBus.
    //
    // Truncating body at 200 chars — same protection the previous
    // QSystemTrayIcon path had against accidentally surfacing long PII
    // (full conversation text, generated code, etc.) in a transient
    // popup.
    const QString safeBody = body.left(200);

    auto* n = new KNotification(eventId);
    n->setComponentName(QStringLiteral("verzeta-studio"));
    n->setTitle(title);
    n->setText(safeBody);
    if (!iconName.isEmpty()) {
        n->setIconName(iconName);
    }
    // Send and self-delete on close.
    n->sendEvent();
}

// ---------------------------------------------------------------------------
// Public API — see header for documentation.
// ---------------------------------------------------------------------------

void NotificationManager::notify(const QString& title,
                                 const QString& body,
                                 const QString& iconName) {
    dispatchEvent(QStringLiteral("info"), title, body, iconName);
}

void NotificationManager::notifyResponseComplete(const QString& conversationTitle,
                                                 const QString& previewText) {
    dispatchEvent(QStringLiteral("responseComplete"),
                  QStringLiteral("Response ready: %1").arg(conversationTitle),
                  previewText.left(100),
                  QStringLiteral("dialog-messages"));
}

void NotificationManager::notifyMediaReady(const QString& type, const QString& conversationTitle) {
    const QString typeCap =
        type.isEmpty() ? QStringLiteral("Media") : (type.at(0).toUpper() + type.mid(1));
    const QString title = QStringLiteral("%1 ready: %2").arg(typeCap, conversationTitle);
    const QString body = QStringLiteral("Open the Artifacts panel to view.");
    const QString iconName = (type == QStringLiteral("image"))
                                 ? QStringLiteral("image-x-generic")
                                 : QStringLiteral("audio-volume-high");
    const QString eventId = (type == QStringLiteral("image")) ? QStringLiteral("imageReady")
                                                              : QStringLiteral("audioReady");
    dispatchEvent(eventId, title, body, iconName);
}

void NotificationManager::notifyUserMentioned(const QString& conversationTitle,
                                              const QString& agentAlias,
                                              const QString& messageBody) {
    const QString title =
        QStringLiteral("@%1 mentioned you in %2").arg(agentAlias, conversationTitle);
    dispatchEvent(QStringLiteral("userMentioned"),
                  title,
                  messageBody.left(140),
                  QStringLiteral("user-online"));
}
