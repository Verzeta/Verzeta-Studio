// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file action-intent-confirmer.h
 * @brief Single-purpose LLM gate confirming whether a stopped assistant turn
 *        ACTUALLY intended to create/write a file as its next step.
 * @layer Service (Chat subsystem)
 * @dependencies Ragp::Service (delegated one-shot completion), QFuture.
 *
 * Why this exists: the 1:1 continuation nudges (write/search claims-vs-fires,
 * deferred-action) are triggered by a cheap REGEX candidate gate over the
 * turn text. English words are ambiguous ("I'll write a function" vs "I'll
 * write the file"), so a regex alone false-positives, and a false positive
 * would fire a spurious retry/nudge. This class is the confirmation step: a
 * SEPARATE, one-shot, NON-cascading LLM call answering one yes/no question:
 * "did this turn promise to create a specific file/artifact next and not do
 * it?". A nudge fires ONLY when the LLM confirms.
 *
 * Provider: it does NOT build its own provider or add a setting. It delegates
 * the one-shot call to Ragp::Service::oneShotCompleteAsync, i.e. uses the SAME
 * provider RAGP is configured with (internal GGUF or remote). It is NOT part
 * of RAGP: no @-mention classification, no cache, no cascade.
 *
 * Degrade contract: if the LLM cannot be reached (no RAGP service, empty
 * answer, error, timeout) the result is Unavailable. The caller treats that
 * as "do NOT fire", because the regex alone is not trustworthy enough to risk
 * a false-positive nudge. Auto-continue is skipped for that turn.
 *
 * Threading: main-thread only; non-blocking. confirmFileCreationIntentAsync returns
 * immediately and resolves via QFuture::finished(); no nested event loops.
 */
#pragma once

#include <functional>
#include <QFuture>
#include <QObject>
#include <QString>

namespace Ragp {
class Service;
}

namespace Chat {

/**
 * @brief One-shot LLM confirmation of a file-creation intent.
 */
class ActionIntentConfirmer : public QObject {
    Q_OBJECT

  public:
    /**
     * @brief Outcome of a confirmation request.
     */
    enum class Result {
        Confirmed,   ///< The LLM confirmed an announced-but-undone file action.
        Rejected,    ///< The LLM said this was not an undone file action.
        Unavailable  ///< Could not reach the LLM; the caller must NOT fire.
    };

    /**
     * @brief Construct against the RAGP service whose configured provider
     *        (internal or remote) issues the one-shot call.
     * @param ragp   Non-owning RAGP service pointer; null ⇒ always Unavailable.
     * @param parent QObject parent.
     */
    explicit ActionIntentConfirmer(Ragp::Service* ragp, QObject* parent = nullptr);
    ~ActionIntentConfirmer() override;

    /**
     * @brief Confirm whether @p turnContent announced an undone file action.
     * @param turnContent     The full terminal assistant reply text.
     * @param executedSummary Human-readable list of the tool calls that
     *                        ALREADY executed earlier in this same turn
     *                        (empty when none). The prompt states it as
     *                        fact so an already-completed action is never
     *                        judged as still pending.
     * @returns A QFuture<Result> that always resolves (Confirmed / Rejected /
     *          Unavailable); never blocks.
     */
    QFuture<Result> confirmFileCreationIntentAsync(const QString& turnContent,
                                                   const QString& executedSummary = {});


    /**
     * @brief Whether a confirmation call can currently be dispatched.
     * @returns true iff a RAGP service is attached (and thus a configured
     *          provider may answer).
     */
    bool canConfirm() const;

    /**
     * @brief Build the one-shot yes/no prompt (exposed for unit testing the
     *        prompt shape).
     * @param turnContent     The terminal assistant reply text.
     * @param executedSummary Tool calls already executed this turn (empty =
     *                        the "no tool ran" framing).
     * @returns The full user-role prompt.
     */
    static QString buildIntentPrompt(const QString& turnContent,
                                     const QString& executedSummary = {});

    /**
     * @brief Map a raw model answer to a Result (exposed for unit testing).
     * @param raw The raw completion text (may contain surrounding prose).
     * @returns Confirmed on a clear YES, Rejected on a clear NO, Unavailable
     *          when empty / unparseable (caller must NOT fire).
     */
    static Result parseIntentAnswer(const QString& raw);

    /**
     * @brief Test seam: replace the LLM call with a synchronous decider.
     * @param fn Called with the turn content; its Result is returned directly
     *           (wrapped in a ready future). Empty std::function restores the
     *           real RAGP-delegated path.
     */
    void setTestDecider(std::function<Result(const QString&)> fn);

  private:
    /**
     * @brief Dispatch @p prompt as a one-shot completion on RAGP's
     *        configured provider and map the raw answer to a Result.
     * @param prompt The fully-built yes/no prompt.
     * @returns A QFuture<Result> resolved from the completion (or
     *          Unavailable when the answer is empty / unparseable).
     */
    QFuture<Result> dispatchOneShot(const QString& prompt);

    Ragp::Service* m_ragp = nullptr;
    std::function<Result(const QString&)> m_testDecider;
};

}  // namespace Chat
