// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file inline-thinking-hoister.cpp
 * @brief Implementation of the streaming `\<think\>` block extractor.
 * @layer Service (Chat subsystem)
 * @dependencies Qt6::Core
 */

#include "inline-thinking-hoister.h"

namespace Chat {

namespace {

const QString kOpener = QStringLiteral("<think>");
const QString kCloser = QStringLiteral("</think>");
const Qt::CaseSensitivity kCS = Qt::CaseInsensitive;

/**
 * @brief Length of the longest suffix of `buf` that is a strict
 *        prefix of `needle`.  Used to hold back the tail of a
 *        chunk whose final characters might be the start of a
 *        partial `\<think\>` / `\</think\>` tag that will complete
 *        when the next chunk arrives.
 */
int longestProperPrefixSuffix(const QString& buf, const QString& needle) {
    const int maxOverlap = qMin(buf.size(), needle.size() - 1);
    for (int len = maxOverlap; len > 0; --len) {
        if (buf.right(len).compare(needle.left(len), kCS) == 0) {
            return len;
        }
    }
    return 0;
}

}  // namespace

InlineThinkingHoister::Result InlineThinkingHoister::feed(const QString& delta) {
    Result out;
    if (delta.isEmpty() && m_buffer.isEmpty()) {
        return out;
    }

    // Concatenate carry-over from the previous feed with the new
    // delta.  All subsequent index arithmetic operates on `work`.
    QString work = m_buffer + delta;
    m_buffer.clear();

    int pos = 0;
    while (pos < work.size()) {
        if (!m_insideThink) {
            // Searching for the next `<think>` opener.
            const int openAt = work.indexOf(kOpener, pos, kCS);
            if (openAt < 0) {
                // No complete opener in the remainder.  Take
                // everything except a possible partial-opener
                // suffix and hold the suffix for next feed.
                const QString tail = work.mid(pos);
                const int holdLen = longestProperPrefixSuffix(tail, kOpener);
                out.content += tail.left(tail.size() - holdLen);
                if (holdLen > 0) {
                    m_buffer = tail.right(holdLen);
                }
                break;
            }
            // Emit everything before the opener; cross into the
            // Inside state.
            out.content += work.mid(pos, openAt - pos);
            pos = openAt + kOpener.size();
            m_insideThink = true;
        } else {
            // Searching for the next `</think>` closer.
            const int closeAt = work.indexOf(kCloser, pos, kCS);
            if (closeAt < 0) {
                // No complete closer.  Append everything except a
                // possible partial-closer suffix to thinking; hold
                // the suffix for next feed.
                const QString tail = work.mid(pos);
                const int holdLen = longestProperPrefixSuffix(tail, kCloser);
                out.thinking += tail.left(tail.size() - holdLen);
                if (holdLen > 0) {
                    m_buffer = tail.right(holdLen);
                }
                break;
            }
            // Append inner text up to the closer; cross back out.
            out.thinking += work.mid(pos, closeAt - pos);
            pos = closeAt + kCloser.size();
            m_insideThink = false;
        }
    }

    return out;
}

void InlineThinkingHoister::reset() {
    m_buffer.clear();
    m_insideThink = false;
}

}  // namespace Chat
