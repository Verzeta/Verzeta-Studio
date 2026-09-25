// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file inline-thinking-hoister.h
 * @brief Streaming-safe extractor that pulls `\<think\>...\</think\>`
 *        blocks out of a content delta stream and routes them to a
 *        parallel thinking-delta stream.
 * @layer Service (Chat subsystem)
 * @dependencies Qt6::Core
 *
 * Stateful, single-stream owner.  One instance per in-flight
 * provider stream, created on the provider as a member and reset
 * automatically when the next stream begins by way of a fresh
 * carry-over buffer.
 *
 * Use case: reasoning-capable models (Qwen 2.5 / 3, DeepSeek-R1
 * quants, gpt-oss-thinking) emit `\<think\>...\</think\>` blocks inline
 * in their content stream when the chat template doesn't suppress
 * them.  Without this hoister those blocks would flow to
 * `Message::content`, get silently stripped by `ContentSanitizer`
 * Stage 5, and the captured reasoning would be lost before the
 * message-bubble disclosure could render it.  With this hoister
 * the inner text is routed to `thinkingDelta` and the surrounding
 * content is unchanged.
 *
 * Threading: single-threaded by design.  Every provider that owns
 * one calls it from the provider's chunk-handler slot on the main
 * thread.  No locking inside the class.
 */

#pragma once

#include <QString>

namespace Chat {

/**
 * @brief Stateful streaming extractor for inline `\<think\>` blocks.
 *
 * `feed(delta)` is called once per provider content chunk.  It
 * returns a `Result` containing the content portion (delta minus
 * any extracted `\<think\>` blocks AND minus a partial-tag suffix
 * held back for the next call) and the thinking portion (the
 * concatenation of inner text from every block fully closed in
 * this feed plus any inner text streamed while a block was open
 * across previous feeds).
 *
 * The hoister handles three streaming patterns:
 *   1. Complete block in one feed: `"hello \<think\>foo\</think\> world"`
 *      → content=`"hello  world"`, thinking=`"foo"`.
 *   2. Opener in one feed, closer in next: feed 1 `"abc \<think\>fo"`
 *      → content=`"abc "`, thinking=`""`, hoister now Inside.
 *      Feed 2 `"o\</think\> def"` → content=`" def"`, thinking=`"foo"`.
 *   3. Partial opener / closer at the tail: feed 1 `"hello <thi"`
 *      → content=`"hello "`, hoister holds `"<thi"` until feed 2.
 *      Feed 2 `"nk>bar\</think\> done"` → content=`" done"`,
 *      thinking=`"bar"`.
 *
 * Reset behaviour: when the owning provider starts a new stream
 * it constructs a new hoister OR calls `reset()` to clear all
 * state (carry-over buffer + Inside flag) so leftover state from
 * a partial previous stream does not bleed into the next.
 *
 * Limitations by design:
 *   - Tags are matched case-insensitively but only the canonical
 *     `\<think\>` / `\</think\>` form is recognised. The sanitiser's
 *     more permissive regex (`<\s*think\s*>`) is a safety net that
 *     catches odd-whitespace variants downstream.
 *   - Nested `\<think\>` blocks are not supported.  The first
 *     `\</think\>` always closes the current block; any subsequent
 *     `\<think\>` opens a fresh one.  Reasoning models that emit
 *     nested thinking are rare; if encountered, the outer-most
 *     pair is captured cleanly and any inner pair becomes part of
 *     the captured thinking text, which is acceptable for v1.0.0.
 */
class InlineThinkingHoister {
  public:
    /**
     * @brief Result of feeding one content delta through the hoister.
     */
    struct Result {
        /** Content portion of the delta, with extracted thinking
         *  blocks removed AND any partial-tag suffix held back for
         *  the next call. */
        QString content;
        /** Thinking text captured during this feed.  Concatenates
         *  inner text of every block fully closed in this feed plus
         *  any inner text streamed while a block was open across
         *  previous feeds. */
        QString thinking;
    };

    /**
     * @brief Feed one delta through the hoister.  Pure transform;
     *        the only mutable state is the hoister instance itself.
     * @param delta Content chunk emitted by the provider for this
     *              token / event.
     * @returns Split result; either side may be empty depending on
     *          what the delta carried.
     */
    Result feed(const QString& delta);

    /**
     * @brief Resets all carry-over state.  Called when the owning
     *        provider starts a new stream so leftover state from a
     *        prior partial stream does not bleed across requests.
     */
    void reset();

    /**
     * @brief True iff the hoister is currently between `\<think\>`
     *        and `\</think\>` and the closer has not arrived yet.
     *        Diagnostic accessor: tests inspect it; production
     *        code does not branch on it.
     * @returns true while the hoister sits inside a `\<think\>` block;
     *          false otherwise (Idle state, including before any
     *          opener has been seen and after every closer).
     */
    bool isInsideThink() const { return m_insideThink; }

  private:
    /** Carry-over buffer for partial tags that may span chunks.
     *  Holds either a partial `\<think\>` opener prefix (when not
     *  inside a block) or a partial `\</think\>` closer prefix (when
     *  inside a block). */
    QString m_buffer;

    /** True iff we have seen `\<think\>` and not yet seen the matching
     *  `\</think\>`.  Mutated only inside `feed()`. */
    bool m_insideThink = false;
};

}  // namespace Chat
