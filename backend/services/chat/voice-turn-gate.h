// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file voice-turn-gate.h
 * @brief The one question the cascade asks about voice: may the next
 *        member's turn start, or is the reply that just landed still
 *        being spoken aloud?
 *
 *        Declared in the chat layer and implemented by the voice
 *        service, so the chat subsystem never depends on voice. The
 *        cascade holds a pointer that is NULL in every build and every
 *        run without an active voice call, which is what keeps the
 *        text-mode path byte-identical.
 * @layer Service (Chat subsystem)
 * @dependencies Qt6::Core (QString) only.
 */

#pragma once

#include <QString>

namespace Chat {

/**
 * @brief Lets a live voice call pace the cascade to speech speed.
 *
 * Without it, a group cascade advances at text speed: the next member
 * generates while the previous reply is still being read aloud, so a
 * spoken interruption arrives after turns that were already decided.
 * Holding the advance until the words have actually been heard keeps
 * the conversation in the order the participants experience it.
 */
class IVoiceTurnGate {
  public:
    virtual ~IVoiceTurnGate() = default;

    /**
     * @brief Whether the cascade must wait before advancing.
     * @param convId The conversation whose turn just completed.
     * @param msgId  The reply that landed; empty when none was
     *               persisted (nothing can be spoken, so nothing waits).
     * @returns True to hold the advance until releaseVoiceHold() is
     *          called with @p msgId. Implementations MUST guarantee
     *          that every true is eventually followed by a release,
     *          including when the audio never plays.
     */
    virtual bool shouldHoldTurn(const QString& convId, const QString& msgId) = 0;
};

}  // namespace Chat
