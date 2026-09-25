// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "helpers/scripted-ragp-backend.h"
#include "services/ragp/iragp-backend.h"
#include "services/ragp/ragp-service.h"
#include "services/ragp/ragp-types.h"

#include <QTest>

#include <memory>
#include <QDeadlineTimer>
#include <QFuture>
#include <QPromise>

using Ragp::Classification;
using Ragp::Intent;
using Ragp::IRagpBackend;
using Ragp::Request;
using Ragp::Service;
using Ragp::Target;

class TestRagpService : public QObject {
    Q_OBJECT

  private:
    static Request
    makeReq(const QString& content, const QString& author, const QStringList& roster) {
        Request r;
        r.content = content;
        r.authorAlias = author;
        r.rosterAliases = roster;
        return r;
    }

    static Classification awaitResult(QFuture<Classification> f) {
        QDeadlineTimer deadline(1000);
        while (!f.isFinished() && !deadline.hasExpired()) {
            QTest::qWait(1);
        }
        Q_ASSERT_X(f.isFinished(),
                   "awaitResult",
                   "future did not finish within 1s — "
                   "continuation likely not wired up");
        return f.result();
    }

  private slots:

    void test_rulesFullyClassify_skipsBackend() {
        auto backend = std::make_unique<ScriptedRagpBackend>();
        ScriptedRagpBackend* raw = backend.get();
        Service svc(std::move(backend));

        const Classification c =
            awaitResult(svc.classify(makeReq(QStringLiteral("Plain message."),
                                             QStringLiteral("Alice"),
                                             {QStringLiteral("Alice"), QStringLiteral("Bob")})));
        QCOMPARE(raw->callCount, 0);
        QCOMPARE(c.source, QStringLiteral("rule"));
        QCOMPARE(c.confidence, 1.0);
    }

    void test_broadcastClassification_skipsBackend() {
        auto backend = std::make_unique<ScriptedRagpBackend>();
        ScriptedRagpBackend* raw = backend.get();
        Service svc(std::move(backend));

        const Classification c =
            awaitResult(svc.classify(makeReq(QStringLiteral("@all please attend"),
                                             QStringLiteral("Alice"),
                                             {QStringLiteral("Alice"), QStringLiteral("Bob")})));
        QCOMPARE(raw->callCount, 0);
        QCOMPARE(c.targets.size(), 1);
        QCOMPARE(c.targets[0].intent, Intent::BROADCAST_REQUEST);
    }

    void test_ambiguousMention_invokesBackend() {
        auto backend = std::make_unique<ScriptedRagpBackend>();
        Classification backendResult;
        backendResult.confidence = 0.8;
        Target t;
        t.alias = QStringLiteral("Bob");
        t.intent = Intent::DELEGATE_RESPONSE;
        backendResult.targets.append(t);
        backend->response = backendResult;
        ScriptedRagpBackend* raw = backend.get();
        Service svc(std::move(backend));

        const Classification c =
            awaitResult(svc.classify(makeReq(QStringLiteral("Hey @Bob, thoughts?"),
                                             QStringLiteral("Alice"),
                                             {QStringLiteral("Alice"), QStringLiteral("Bob")})));
        QCOMPARE(raw->callCount, 1);
        QCOMPARE(c.targets.size(), 1);
        QCOMPARE(c.targets[0].intent, Intent::DELEGATE_RESPONSE);
    }

    void test_unsureBackendWithRuleTargets_prefersRules() {
        auto backend = std::make_unique<ScriptedRagpBackend>();
        Classification junk;
        junk.confidence = 0.0;
        for (const QString& a : {QStringLiteral("Kate"),
                                 QStringLiteral("Marco"),
                                 QStringLiteral("Nico"),
                                 QStringLiteral("Robin")}) {
            Target t;
            t.alias = a;
            t.intent = Intent::DELEGATE_RESPONSE;
            junk.targets.append(t);
        }
        backend->response = junk;
        ScriptedRagpBackend* raw = backend.get();
        Service svc(std::move(backend));

        const Classification c = awaitResult(
            svc.classify(makeReq(QStringLiteral("@Marco, could you please take the lead on "
                                                "the first step?"),
                                 QStringLiteral("Robin"),
                                 {QStringLiteral("Kate"),
                                  QStringLiteral("Marco"),
                                  QStringLiteral("Nico"),
                                  QStringLiteral("Robin")})));
        QCOMPARE(raw->callCount, 1);
        QVERIFY2(c.source.startsWith(QStringLiteral("rule-preferred-after-unsure")),
                 qPrintable(c.source));
        QCOMPARE(c.targets.size(), 1);
        QCOMPARE(c.targets[0].alias, QStringLiteral("Marco"));
    }

    void test_unsureBackendWithoutRuleTargets_keptAsIs() {
        auto backend = std::make_unique<ScriptedRagpBackend>();
        Classification unsure;
        unsure.confidence = 0.3;
        Target t;
        t.alias = QStringLiteral("Bob");
        t.intent = Intent::DELEGATE_RESPONSE;
        unsure.targets.append(t);
        backend->response = unsure;
        Service svc(std::move(backend));

        Request req = makeReq(QStringLiteral("I'll draft pricing.md next."),
                              QStringLiteral("Alice"),
                              {QStringLiteral("Alice"), QStringLiteral("Bob")});
        req.wantsPendingActionVerdict = true;

        const Classification c = awaitResult(svc.classify(req));
        QCOMPARE(c.targets.size(), 1);
        QCOMPARE(c.confidence, 0.3);
        QVERIFY(!c.source.startsWith(QStringLiteral("rule-preferred")));
    }

    void test_fabricatedTargets_fallBackToRules() {
        auto backend = std::make_unique<ScriptedRagpBackend>();
        Classification regurgitated;
        regurgitated.confidence = 0.95;
        for (const QString& a : {QStringLiteral("Kate"),
                                 QStringLiteral("Rob"),
                                 QStringLiteral("Sam"),
                                 QStringLiteral("Dan")}) {
            Target t;
            t.alias = a;
            t.intent = Intent::PLURAL_ADDRESS;
            regurgitated.targets.append(t);
        }
        backend->response = regurgitated;
        Service svc(std::move(backend));

        const Classification c = awaitResult(
            svc.classify(makeReq(QStringLiteral("@Robin, that preliminary scope document "
                                                "is crucial — could we lock it down?"),
                                 QStringLiteral("Kate"),
                                 {QStringLiteral("Kate"),
                                  QStringLiteral("Marco"),
                                  QStringLiteral("Nico"),
                                  QStringLiteral("Robin")})));
        QVERIFY2(c.source.startsWith(QStringLiteral("rule-fallback-after:")), qPrintable(c.source));
        QCOMPARE(c.targets.size(), 1);
        QCOMPARE(c.targets[0].alias, QStringLiteral("Robin"));
    }

    void test_partiallyFabricated_keepsValidTargets() {
        auto backend = std::make_unique<ScriptedRagpBackend>();
        Classification mixed;
        mixed.confidence = 0.9;
        Target good;
        good.alias = QStringLiteral("Marco");
        good.intent = Intent::DELEGATE_RESPONSE;
        mixed.targets.append(good);
        Target junk;
        junk.alias = QStringLiteral("Zed");
        junk.intent = Intent::DELEGATE_RESPONSE;
        mixed.targets.append(junk);
        backend->response = mixed;
        Service svc(std::move(backend));

        const Classification c =
            awaitResult(svc.classify(makeReq(QStringLiteral("Hey @Marco, thoughts on the scope?"),
                                             QStringLiteral("Kate"),
                                             {QStringLiteral("Kate"), QStringLiteral("Marco")})));
        QCOMPARE(c.targets.size(), 1);
        QCOMPARE(c.targets[0].alias, QStringLiteral("Marco"));
        QVERIFY(!c.source.startsWith(QStringLiteral("rule-")));
    }

    void test_fallback_preservesRequestedVerdict() {
        auto backend = std::make_unique<ScriptedRagpBackend>();
        Classification junk;
        junk.confidence = 0.95;
        junk.selfPendingAction = true;
        junk.pendingActionHint = QStringLiteral("write style guide");
        Target t;
        t.alias = QStringLiteral("Amy");
        t.intent = Intent::DELEGATE_TASK;
        junk.targets.append(t);
        backend->response = junk;
        Service svc(std::move(backend));

        Request req = makeReq(QStringLiteral("I am calling write_file now! Please keep "
                                             "an eye on the canvas."),
                              QStringLiteral("Kate"),
                              {QStringLiteral("Kate"), QStringLiteral("Marco")});
        req.wantsPendingActionVerdict = true;

        const Classification c = awaitResult(svc.classify(req));
        QVERIFY2(c.source.startsWith(QStringLiteral("rule-fallback-after:")), qPrintable(c.source));
        QVERIFY(c.targets.isEmpty());
        QVERIFY2(c.selfPendingAction, "the requested verdict must survive the fallback");
        QCOMPARE(c.pendingActionHint, QStringLiteral("write style guide"));

        auto backend2 = std::make_unique<ScriptedRagpBackend>();
        backend2->response = junk;
        Service svc2(std::move(backend2));
        Request req2 = req;
        req2.wantsPendingActionVerdict = false;
        const Classification c2 = awaitResult(svc2.classify(req2));
        QVERIFY(!c2.selfPendingAction);
    }

    void test_delegateVerdict_rosterValidatedAndPreserved() {
        auto backend = std::make_unique<ScriptedRagpBackend>();
        Classification ans;
        ans.confidence = 0.95;
        ans.pendingDelegateAlias = QStringLiteral("Robin");
        ans.pendingActionHint = QStringLiteral("call complete_task");
        backend->response = ans;
        Service svc(std::move(backend));

        Request req = makeReq(QStringLiteral("@Robin, thank you for summarizing! Please "
                                             "go ahead and call complete_task."),
                              QStringLiteral("Marco"),
                              {QStringLiteral("Marco"), QStringLiteral("Robin")});
        req.wantsPendingActionVerdict = true;

        const Classification c = awaitResult(svc.classify(req));
        QCOMPARE(c.pendingDelegateAlias, QStringLiteral("Robin"));
        QCOMPARE(c.pendingActionHint, QStringLiteral("call complete_task"));
        QVERIFY(!c.selfPendingAction);
    }

    void test_delegateVerdict_fabricatedAliasCleared_authorIsSelf() {
        auto backend = std::make_unique<ScriptedRagpBackend>();
        Classification fab;
        fab.confidence = 0.95;
        fab.pendingDelegateAlias = QStringLiteral("Zoe");
        fab.pendingActionHint = QStringLiteral("deploy");
        backend->response = fab;
        Service svc(std::move(backend));
        Request req = makeReq(QStringLiteral("please run the tests"),
                              QStringLiteral("Marco"),
                              {QStringLiteral("Marco"), QStringLiteral("Robin")});
        req.wantsPendingActionVerdict = true;
        const Classification c = awaitResult(svc.classify(req));
        QVERIFY2(c.pendingDelegateAlias.isEmpty(),
                 "a delegate alias outside the roster must be cleared");

        auto backend2 = std::make_unique<ScriptedRagpBackend>();
        Classification selfish;
        selfish.confidence = 0.9;
        selfish.pendingDelegateAlias = QStringLiteral("Marco");
        selfish.pendingActionHint = QStringLiteral("write pricing.md");
        backend2->response = selfish;
        Service svc2(std::move(backend2));
        const Classification c2 = awaitResult(svc2.classify(req));
        QVERIFY(c2.pendingDelegateAlias.isEmpty());
        QVERIFY2(c2.selfPendingAction, "author-as-delegate converts to the self verdict");
        QCOMPARE(c2.pendingActionHint, QStringLiteral("write pricing.md"));
    }

    void test_delegateVerdict_survivesFallback_clearedWhenUnrequested() {
        auto backend = std::make_unique<ScriptedRagpBackend>();
        Classification junk;
        junk.confidence = 0.95;
        junk.pendingDelegateAlias = QStringLiteral("Robin");
        junk.pendingActionHint = QStringLiteral("call complete_task");
        Target t;
        t.alias = QStringLiteral("Amy");
        t.intent = Intent::DELEGATE_TASK;
        junk.targets.append(t);
        backend->response = junk;
        Service svc(std::move(backend));

        Request req = makeReq(QStringLiteral("please finish it"),
                              QStringLiteral("Marco"),
                              {QStringLiteral("Marco"), QStringLiteral("Robin")});
        req.wantsPendingActionVerdict = true;
        const Classification c = awaitResult(svc.classify(req));
        QVERIFY2(c.source.startsWith(QStringLiteral("rule-fallback-after:")), qPrintable(c.source));
        QCOMPARE(c.pendingDelegateAlias, QStringLiteral("Robin"));

        auto backend2 = std::make_unique<ScriptedRagpBackend>();
        Classification vol;
        vol.confidence = 0.9;
        vol.pendingDelegateAlias = QStringLiteral("Robin");
        backend2->response = vol;
        Service svc2(std::move(backend2));
        Request req2 = req;
        req2.wantsPendingActionVerdict = false;
        const Classification c2 = awaitResult(svc2.classify(req2));
        QVERIFY(c2.pendingDelegateAlias.isEmpty());
    }

    void test_cacheHit_backendInvokedOnce() {
        auto backend = std::make_unique<ScriptedRagpBackend>();
        Classification backendResult;
        backendResult.confidence = 0.8;
        Target t;
        t.alias = QStringLiteral("Bob");
        t.intent = Intent::DELEGATE_RESPONSE;
        backendResult.targets.append(t);
        backend->response = backendResult;
        ScriptedRagpBackend* raw = backend.get();
        Service svc(std::move(backend));

        const Request r = makeReq(QStringLiteral("Hey @Bob?"),
                                  QStringLiteral("Alice"),
                                  {QStringLiteral("Alice"), QStringLiteral("Bob")});

        awaitResult(svc.classify(r));
        QCOMPARE(raw->callCount, 1);

        const Classification c2 = awaitResult(svc.classify(r));
        QCOMPARE(raw->callCount, 1);
        QVERIFY(c2.fromCache);
    }

    void test_backendUnavailable_gracefulDegradation() {
        auto backend = std::make_unique<ScriptedRagpBackend>();
        backend->available = false;
        ScriptedRagpBackend* raw = backend.get();
        Service svc(std::move(backend));

        const Classification c =
            awaitResult(svc.classify(makeReq(QStringLiteral("Hey @Bob?"),
                                             QStringLiteral("Alice"),
                                             {QStringLiteral("Alice"), QStringLiteral("Bob")})));
        QCOMPARE(raw->callCount, 0);
        QVERIFY(c.latencyMs >= 0);
    }

    void test_clearCache() {
        auto backend = std::make_unique<ScriptedRagpBackend>();
        Classification backendResult;
        backendResult.confidence = 0.8;
        Target t;
        t.alias = QStringLiteral("Bob");
        t.intent = Intent::DELEGATE_RESPONSE;
        backendResult.targets.append(t);
        backend->response = backendResult;
        ScriptedRagpBackend* raw = backend.get();
        Service svc(std::move(backend));

        const Request r = makeReq(QStringLiteral("Hey @Bob?"),
                                  QStringLiteral("Alice"),
                                  {QStringLiteral("Alice"), QStringLiteral("Bob")});

        awaitResult(svc.classify(r));
        QCOMPARE(raw->callCount, 1);
        svc.clearCache();
        awaitResult(svc.classify(r));
        QCOMPARE(raw->callCount, 2);
    }

    void test_backendFailure_fallsBackToRuleResult() {
        auto backend = std::make_unique<ScriptedRagpBackend>();
        Classification failed;
        failed.confidence = 0.0;
        failed.source = QStringLiteral("mock:failed");
        backend->response = failed;
        ScriptedRagpBackend* raw = backend.get();
        Service svc(std::move(backend));

        const Classification c =
            awaitResult(svc.classify(makeReq(QStringLiteral("Hey @Bob, thoughts?"),
                                             QStringLiteral("Alice"),
                                             {QStringLiteral("Alice"), QStringLiteral("Bob")})));

        QCOMPARE(raw->callCount, 1);
        QCOMPARE(c.targets.size(), 1);
        QCOMPARE(c.targets[0].alias, QStringLiteral("Bob"));
        QCOMPARE(c.targets[0].intent, Intent::UNKNOWN);
        QVERIFY(c.source.startsWith(QStringLiteral("rule-fallback-after:")));
    }

    void test_backendFailure_notCached() {
        auto backend = std::make_unique<ScriptedRagpBackend>();
        Classification failed;
        failed.confidence = 0.0;
        backend->response = failed;
        ScriptedRagpBackend* raw = backend.get();
        Service svc(std::move(backend));

        const Request r = makeReq(QStringLiteral("Hey @Bob"),
                                  QStringLiteral("Alice"),
                                  {QStringLiteral("Alice"), QStringLiteral("Bob")});

        awaitResult(svc.classify(r));
        QCOMPARE(raw->callCount, 1);
        awaitResult(svc.classify(r));
        QCOMPARE(raw->callCount, 2);
    }

    void test_latencyReported() {
        auto backend = std::make_unique<ScriptedRagpBackend>();
        Service svc(std::move(backend));

        const Classification c = awaitResult(svc.classify(
            makeReq(QStringLiteral("hi"), QStringLiteral("Alice"), {QStringLiteral("Alice")})));
        QVERIFY(c.latencyMs >= 0);
    }
};

QTEST_MAIN(TestRagpService)
#include "test-ragp-service.moc"
