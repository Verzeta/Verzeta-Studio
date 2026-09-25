// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file action-intent-confirmer.cpp
 * @brief Implementation of Chat::ActionIntentConfirmer.
 * @layer Service (Chat subsystem)
 * @dependencies Ragp::Service::oneShotCompleteAsync, QFutureWatcher, QPromise.
 */

#include "action-intent-confirmer.h"

#include "../ragp/ragp-service.h"

#include <QFutureWatcher>
#include <QPromise>

namespace Chat {

namespace {

QFuture<ActionIntentConfirmer::Result> readyResult(ActionIntentConfirmer::Result r) {
    QPromise<ActionIntentConfirmer::Result> p;
    p.start();
    p.addResult(r);
    p.finish();
    return p.future();
}

}  // namespace

ActionIntentConfirmer::ActionIntentConfirmer(Ragp::Service* ragp, QObject* parent)
    : QObject(parent), m_ragp(ragp) {}

ActionIntentConfirmer::~ActionIntentConfirmer() = default;

bool ActionIntentConfirmer::canConfirm() const {
    return m_testDecider || m_ragp != nullptr;
}

void ActionIntentConfirmer::setTestDecider(std::function<Result(const QString&)> fn) {
    m_testDecider = std::move(fn);
}

namespace {

/**
 * @brief Render the executed-tools CONTEXT block for the intent prompt.
 * @param executedSummary Human-readable executed-call list (may be empty).
 * @param subject         The actor wording (currently "the assistant").
 * @returns A neutral context paragraph. It must NOT imply that a
 *          past-tense summary of earlier work is an unbacked claim. The
 *          old "nothing ran this turn" framing made the judge treat a
 *          wrap-up like "I have finished editing X" as a pending action.
 */
QString executedFactBlock(const QString& executedSummary, const QString& subject) {
    if (executedSummary.trimmed().isEmpty()) {
        return QStringLiteral("CONTEXT: no tool ran during THIS turn. This alone does NOT mean "
                              "%1 owes an action — the reply may be summarising work done "
                              "earlier, answering, or asking you something. Decide from the "
                              "reply's OWN intent below.\n\n")
            .arg(subject);
    }
    return QStringLiteral("CONTEXT: earlier in this SAME turn %1 already ran: %2. If the reply "
                          "only describes or summarises that completed work, answer NO. Answer "
                          "YES only for a DIFFERENT, still-unperformed action it commits to "
                          "doing next.\n\n")
        .arg(subject, executedSummary);
}

}  // namespace

QString ActionIntentConfirmer::buildIntentPrompt(const QString& turnContent,
                                                 const QString& executedSummary) {
    // CONSERVATIVE, tense-aware detector. The nudge only helps when the
    // reply COMMITS to an action it will do NEXT and then stopped; firing it
    // on a question, an offer, or a summary of finished work derails the
    // assistant (it apologises and loops). So the default is NO, and YES
    // needs an unambiguous FUTURE, not-yet-done commitment. Naming a
    // tool/file is NOT enough — a wrap-up ("I have finished editing X … can
    // we close this?") names files but is past-tense + a question. The
    // reframed CONTEXT block no longer implies a past-tense summary is an
    // unbacked claim (the previous "nothing ran this turn" framing was the
    // bias that made the judge nudge conclusions).
    //
    // Keep the call cheap: a long reply's actionable announcement is at the
    // END, so send head + tail rather than the full body.
    QString body = turnContent;
    constexpr int kHead = 400;
    constexpr int kTail = 1200;
    if (body.length() > kHead + kTail + 40) {
        body = body.left(kHead) + QStringLiteral("\n…[middle elided]…\n") + body.right(kTail);
    }

    return QStringLiteral("You are a strict, single-purpose DETECTOR for a coding assistant.\n"
                          "%2"
                          "Decide ONE thing: did this reply COMMIT to a specific tool action it "
                          "will perform NEXT (now) and has NOT yet performed — so it should be "
                          "told to go ahead and run it?\n\n"
                          "Judge the MEANING in ANY language, never specific words.\n\n"
                          "Answer NO — this is the DEFAULT; when in ANY doubt, answer NO — if "
                          "the reply:\n"
                          "  - asks the user anything, requests permission or confirmation, "
                          "offers or proposes an action, or presents options to choose. It is "
                          "WAITING for the user (e.g. \"Shall I create notes.md?\", \"can we "
                          "close this?\"). Naming a tool or file inside a question or offer is "
                          "still WAITING;\n"
                          "  - describes work as ALREADY DONE, or summarises / concludes — "
                          "past-tense like \"I have finished editing report.md\", \"I created "
                          "X\", \"the document now contains …\", a status recap or a project "
                          "wrap-up — EVEN IF it names files or tools;\n"
                          "  - apologises, explains a failure, or declines to run a tool;\n"
                          "  - is a finished answer or general discussion.\n"
                          "Answer YES ONLY if the reply's own words commit to performing a "
                          "specific tool/file action IMMEDIATELY and it is not yet done — a "
                          "FUTURE commitment such as \"I'll create pricing.md now\", \"Calling "
                          "write_file…\", \"I'll use edit_canvas to update it\", or content "
                          "announced then cut off (the reply ends at \"Here is the draft:\" "
                          "with nothing after it).\n\n"
                          "Reply with EXACTLY ONE WORD: YES or NO.\n\n"
                          "ASSISTANT REPLY:\n\"\"\"\n%1\n\"\"\"\n\nAnswer (YES or NO):")
        .arg(body, executedFactBlock(executedSummary, QStringLiteral("the assistant")));
}

ActionIntentConfirmer::Result ActionIntentConfirmer::parseIntentAnswer(const QString& raw) {
    const QString t = raw.trimmed().toUpper();
    if (t.isEmpty()) {
        return Result::Unavailable;  // could not determine → caller won't fire
    }
    if (t.startsWith(QStringLiteral("YES")))
        return Result::Confirmed;
    if (t.startsWith(QStringLiteral("NO")))
        return Result::Rejected;
    // Fallback for a wrapped answer: a clear YES with no NO is Confirmed;
    // any NO present is Rejected; otherwise unparseable → do NOT fire.
    const bool hasYes = t.contains(QStringLiteral("YES"));
    const bool hasNo = t.contains(QStringLiteral("NO"));
    if (hasYes && !hasNo)
        return Result::Confirmed;
    if (hasNo)
        return Result::Rejected;
    return Result::Unavailable;
}

QFuture<ActionIntentConfirmer::Result>
ActionIntentConfirmer::confirmFileCreationIntentAsync(const QString& turnContent,
                                                      const QString& executedSummary) {
    // Test seam: synchronous decider, no LLM.
    if (m_testDecider) {
        return readyResult(m_testDecider(turnContent));
    }
    // No RAGP service → cannot determine → caller must NOT fire.
    if (!m_ragp) {
        return readyResult(Result::Unavailable);
    }
    return dispatchOneShot(buildIntentPrompt(turnContent, executedSummary));
}

QFuture<ActionIntentConfirmer::Result>
ActionIntentConfirmer::dispatchOneShot(const QString& prompt) {
    auto promise = std::make_shared<QPromise<Result>>();
    promise->start();

    auto* watcher = new QFutureWatcher<QString>(this);
    connect(watcher, &QFutureWatcher<QString>::finished, this, [promise, watcher]() {
        const QString raw = watcher->future().resultCount() > 0 ? watcher->result() : QString();
        promise->addResult(parseIntentAnswer(raw));
        promise->finish();
        watcher->deleteLater();
    });
    // Delegate the one-shot call to the provider RAGP is configured with
    // (internal GGUF or remote) — no separate provider, no new setting.
    watcher->setFuture(m_ragp->oneShotCompleteAsync(prompt));
    return promise->future();
}

}  // namespace Chat
