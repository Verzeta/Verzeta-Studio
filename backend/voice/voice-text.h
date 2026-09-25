// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file voice-text.h
 * @brief Turns an assistant message's markdown into speakable text and
 *        splits it into sentences, so synthesis can start on the first
 *        sentence while the rest is still being prepared.
 *
 *        Pure functions with no state and no Qt GUI dependency, so the
 *        speech rules are unit-tested directly rather than observed
 *        through the audio pipeline.
 * @layer Utility (Voice)
 * @dependencies Qt6::Core only.
 */

#pragma once

#include <QString>
#include <QStringList>

namespace Verzeta::Voice {

/**
 * @brief Converts markdown prose to plain speakable text.
 *
 * Fenced and indented code blocks are replaced by the single spoken cue
 * "Code omitted." because reading punctuation aloud is useless. The
 * indented rule is list- and table-aware: an indented run counts as
 * code only when it starts after a blank line and is not a bullet, a
 * numbered item or a table row, so nested lists are spoken as prose.
 * Tables (markdown pipe tables in both forms, and HTML tables) become
 * a one-line summary of what the table is about and how many rows it
 * has. Other HTML tags are dropped and their text kept. Inline
 * emphasis, headings, list bullets, links (the label is kept, the URL
 * dropped) and inline code backticks are unwrapped; `@Alias` mentions
 * lose the at-sign so they are pronounced as names; emoji and other
 * symbol characters are removed; whitespace is collapsed.
 *
 * Free-form ASCII or box-drawing art is NOT detected; it has no reliable
 * signature and guessing at one would eat real prose.
 *
 * @param markdown The message body as stored in the conversation.
 * @returns Plain text ready for synthesis; empty when nothing is left
 *          to say (a tool-only or code-only message).
 */
QString toSpeakableText(const QString& markdown);

/**
 * @brief Splits speakable text into sentence-sized synthesis chunks.
 *
 * Splitting happens on sentence-ending punctuation followed by
 * whitespace. Very short fragments are merged into the next chunk so
 * abbreviations do not spray one-word utterances at the synthesizer.
 *
 * @param text Plain text from toSpeakableText().
 * @returns Ordered chunks; a single chunk when the text has no
 *          sentence break; empty for empty input.
 */
QStringList splitIntoSentences(const QString& text);

}  // namespace Verzeta::Voice
