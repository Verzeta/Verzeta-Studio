// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "services/ragp/iragp-backend.h"
#include "services/ragp/local-llama-backend.h"
#include "services/ragp/ragp-types.h"

#include <QTemporaryDir>
#include <QtTest>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFuture>
#include <QFutureWatcher>
#include <QSignalSpy>
#include <QString>

class TestLocalLlamaBackend : public QObject {
    Q_OBJECT

  private:
    static QString bogusModelPath() {
        return QStringLiteral("/definitely/does/not/exist/fake.gguf");
    }

    static Ragp::Request makeSampleRequest() {
        Ragp::Request req;
        req.content = QStringLiteral("@Alice please review");
        req.authorAlias = QStringLiteral("bob");
        req.rosterAliases = {QStringLiteral("alice"), QStringLiteral("bob")};
        return req;
    }

  private slots:


    void testConstructionAndDestructionClean() {
        QElapsedTimer timer;
        timer.start();
        { Ragp::LocalLlamaBackend backend(bogusModelPath(), nullptr); }
        QVERIFY2(timer.elapsed() < 6000,
                 qPrintable(QStringLiteral("Destruction took %1 ms — should be near-instant "
                                           "when no classify has run")
                                .arg(timer.elapsed())));
    }

    void testBackendNameContainsFilename() {
        Ragp::LocalLlamaBackend backend(QStringLiteral("/tmp/phi-4-mini-q4.gguf"), nullptr);
        QCOMPARE(backend.backendName(), QStringLiteral("local:llama.cpp:phi-4-mini-q4.gguf"));
    }

    void testBackendNameEmptyPath() {
        Ragp::LocalLlamaBackend backend(QString{}, nullptr);
        QCOMPARE(backend.backendName(), QStringLiteral("local:llama.cpp"));
    }

    void testIsAvailableFalseBeforeLoad() {
        Ragp::LocalLlamaBackend backend(bogusModelPath(), nullptr);
        QCOMPARE(backend.isAvailable(), false);
    }


    void testLoadFailedSignalOnMissingFile() {
        Ragp::LocalLlamaBackend backend(bogusModelPath(), nullptr);
        backend.setTimeoutMs(500);

        QSignalSpy failSpy(&backend, &Ragp::LocalLlamaBackend::loadFailed);
        QSignalSpy succSpy(&backend, &Ragp::LocalLlamaBackend::loadSucceeded);

        QFuture<Ragp::Classification> fut = backend.classifyAsync(makeSampleRequest());

        QVERIFY(failSpy.wait(3000));
        QCOMPARE(succSpy.count(), 0);

        QFutureWatcher<Ragp::Classification> watcher;
        watcher.setFuture(fut);
        QSignalSpy finSpy(&watcher, &QFutureWatcher<Ragp::Classification>::finished);
        QVERIFY(finSpy.wait(3000));

        QCOMPARE(fut.resultCount(), 1);
        const Ragp::Classification c = fut.result();
        QVERIFY(c.targets.isEmpty());
        QCOMPARE(c.confidence, 0.0);
        QVERIFY2(c.source.startsWith(QStringLiteral("local:")), qPrintable(c.source));

        QCOMPARE(backend.isAvailable(), false);
    }

    void testLoadFailedBackendStillAcceptsFurtherClassifies() {
        Ragp::LocalLlamaBackend backend(bogusModelPath(), nullptr);
        backend.setTimeoutMs(500);

        QSignalSpy failSpy(&backend, &Ragp::LocalLlamaBackend::loadFailed);
        QFuture<Ragp::Classification> first = backend.classifyAsync(makeSampleRequest());
        QVERIFY(failSpy.wait(3000));

        QFutureWatcher<Ragp::Classification> w1;
        w1.setFuture(first);
        QSignalSpy w1Spy(&w1, &QFutureWatcher<Ragp::Classification>::finished);
        QVERIFY(w1Spy.wait(3000));

        QFuture<Ragp::Classification> second = backend.classifyAsync(makeSampleRequest());
        QFutureWatcher<Ragp::Classification> w2;
        w2.setFuture(second);
        QSignalSpy w2Spy(&w2, &QFutureWatcher<Ragp::Classification>::finished);
        QVERIFY(w2Spy.wait(3000));

        QCOMPARE(second.resultCount(), 1);
        const Ragp::Classification c = second.result();
        QVERIFY(c.targets.isEmpty());
        QCOMPARE(c.confidence, 0.0);
    }


    void testClassifyAlwaysResolvesBeforeTimeoutPlusSlack() {
        Ragp::LocalLlamaBackend backend(bogusModelPath(), nullptr);
        const int timeoutMs = 400;
        backend.setTimeoutMs(timeoutMs);

        QElapsedTimer timer;
        timer.start();

        QFuture<Ragp::Classification> fut = backend.classifyAsync(makeSampleRequest());

        QFutureWatcher<Ragp::Classification> watcher;
        watcher.setFuture(fut);
        QSignalSpy finSpy(&watcher, &QFutureWatcher<Ragp::Classification>::finished);
        QVERIFY(finSpy.wait(3000));

        const qint64 elapsed = timer.elapsed();
        QVERIFY2(elapsed < 3000,
                 qPrintable(QStringLiteral("Future took %1 ms to resolve").arg(elapsed)));

        QCOMPARE(fut.resultCount(), 1);
        QVERIFY(fut.result().targets.isEmpty());
    }


    void testConcurrentClassifiesAllResolve() {
        Ragp::LocalLlamaBackend backend(bogusModelPath(), nullptr);
        backend.setTimeoutMs(500);

        const int N = 10;
        QList<QFuture<Ragp::Classification>> futures;
        futures.reserve(N);
        for (int i = 0; i < N; ++i) {
            futures.append(backend.classifyAsync(makeSampleRequest()));
        }

        QTRY_VERIFY_WITH_TIMEOUT(
            std::all_of(futures.begin(),
                        futures.end(),
                        [](const QFuture<Ragp::Classification>& f) { return f.isFinished(); }),
            5000);

        for (int i = 0; i < N; ++i) {
            QCOMPARE(futures[i].resultCount(), 1);
            const Ragp::Classification c = futures[i].result();
            QVERIFY(!c.source.isEmpty());
        }
    }


    void testDestructionDuringPendingClassify() {
        QFuture<Ragp::Classification> escapedFuture;
        const int timeoutMs = 500;

        {
            Ragp::LocalLlamaBackend backend(bogusModelPath(), nullptr);
            backend.setTimeoutMs(timeoutMs);
            escapedFuture = backend.classifyAsync(makeSampleRequest());
        }

        QFutureWatcher<Ragp::Classification> watcher;
        watcher.setFuture(escapedFuture);
        QSignalSpy finSpy(&watcher, &QFutureWatcher<Ragp::Classification>::finished);
        QVERIFY(finSpy.wait(3000));

        QCOMPARE(escapedFuture.resultCount(), 1);
        const QString source = escapedFuture.result().source;
        QVERIFY2(source.startsWith(QStringLiteral("local:")), qPrintable(source));
    }


    void testRepeatedLifecycle() {
        for (int i = 0; i < 5; ++i) {
            Ragp::LocalLlamaBackend backend(bogusModelPath(), nullptr);
        }
        QVERIFY(true);
    }
};

QTEST_MAIN(TestLocalLlamaBackend)
#include "test-local-llama-backend.moc"
