// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: GPL-3.0-or-later


#include "../../backend/utils/contention-ledger.h"

#include <QThread>
#include <QtTest>

#include <atomic>
#include <QLoggingCategory>

namespace {

struct LogCapture {
    static QStringList lines;
    static QtMessageHandler previous;

    static void handler(QtMsgType type, const QMessageLogContext& ctx, const QString& msg) {
        if (ctx.category && qstrcmp(ctx.category, "verzeta.ledger") == 0) {
            lines << msg;
            return;
        }
        if (previous)
            previous(type, ctx, msg);
    }

    static void install() {
        lines.clear();
        previous = qInstallMessageHandler(&handler);
    }
    static void remove() {
        qInstallMessageHandler(previous);
        previous = nullptr;
    }
};
QStringList LogCapture::lines;
QtMessageHandler LogCapture::previous = nullptr;

}  // namespace

class TestContentionLedger : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase() {
        QLoggingCategory::setFilterRules(QStringLiteral("verzeta.ledger.debug=true"));
    }

    void init() { QCOMPARE(ContentionLedger::openCount(), 0); }

    void test_lifecycle_and_logLine() {
        LogCapture::install();
        const quint64 id = ContentionLedger::begin(ContentionLedger::CallClass::Ragp,
                                                   QStringLiteral("ollama"),
                                                   QStringLiteral("gemma4:e4b"));
        QVERIFY(id != 0);
        QCOMPARE(ContentionLedger::openCount(), 1);
        ContentionLedger::markDispatched(id);
        ContentionLedger::markFirstToken(id);
        ContentionLedger::end(id, true, QStringLiteral("rule"));
        LogCapture::remove();

        QCOMPARE(ContentionLedger::openCount(), 0);
        QCOMPARE(LogCapture::lines.size(), 1);
        const QString& line = LogCapture::lines.first();
        QVERIFY(line.contains(QStringLiteral("LEDGER class=ragp")));
        QVERIFY(line.contains(QStringLiteral("provider=ollama")));
        QVERIFY(line.contains(QStringLiteral("model=gemma4:e4b")));
        QVERIFY(line.contains(QStringLiteral("ok=1")));
        QVERIFY(line.contains(QStringLiteral("note=rule")));
        QVERIFY(!line.contains(QStringLiteral("queue_ms=-1")));
    }

    void test_endIsIdempotent_andUnknownIdsAreNoOps() {
        LogCapture::install();
        const quint64 id = ContentionLedger::begin(
            ContentionLedger::CallClass::Turn, QStringLiteral("ollama"), QStringLiteral("m"));
        ContentionLedger::end(id, true);
        ContentionLedger::end(id, false, QStringLiteral("released"));
        ContentionLedger::end(id, false);
        ContentionLedger::markDispatched(999999);
        ContentionLedger::markFirstToken(999999);
        ContentionLedger::end(999999, true);
        LogCapture::remove();

        QCOMPARE(LogCapture::lines.size(), 1);
        QVERIFY(LogCapture::lines.first().contains(QStringLiteral("ok=1")));
        QCOMPARE(ContentionLedger::openCount(), 0);
    }

    void test_concurrentSnapshot_capturesOverlap() {
        LogCapture::install();
        const quint64 turn = ContentionLedger::begin(
            ContentionLedger::CallClass::Turn, QStringLiteral("ollama"), QStringLiteral("m"));
        const quint64 embed = ContentionLedger::begin(
            ContentionLedger::CallClass::Embed, QStringLiteral("192.0.2.10:11434"), QString());
        ContentionLedger::end(embed, true);
        ContentionLedger::end(turn, true);
        LogCapture::remove();

        QCOMPARE(LogCapture::lines.size(), 2);
        QVERIFY(LogCapture::lines.at(0).contains(QStringLiteral("class=embed")));
        QVERIFY(LogCapture::lines.at(0).contains(QStringLiteral("concurrent=[turn:ollama]")));
        QVERIFY(LogCapture::lines.at(1).contains(QStringLiteral("concurrent=[]")));
    }

    void test_neverDispatched_reportsQueueMinusOne() {
        LogCapture::install();
        const quint64 id = ContentionLedger::begin(
            ContentionLedger::CallClass::Summary, QStringLiteral("ollama"), QStringLiteral("m"));
        ContentionLedger::end(id, false, QStringLiteral("aborted"));
        LogCapture::remove();
        QCOMPARE(LogCapture::lines.size(), 1);
        QVERIFY(LogCapture::lines.first().contains(QStringLiteral("queue_ms=-1")));
        QVERIFY(LogCapture::lines.first().contains(QStringLiteral("ttft_ms=-1")));
    }

    void test_crossThread_beginEnd_smoke() {
        std::atomic<bool> ok{true};
        auto* worker = QThread::create([&ok]() {
            for (int i = 0; i < 500; ++i) {
                const quint64 id = ContentionLedger::begin(
                    ContentionLedger::CallClass::Embed, QStringLiteral("w"), QString());
                ContentionLedger::markDispatched(id);
                ContentionLedger::end(id, true);
                if (ContentionLedger::openCount() < 0)
                    ok = false;
            }
        });
        worker->start();
        for (int i = 0; i < 500; ++i) {
            const quint64 id = ContentionLedger::begin(
                ContentionLedger::CallClass::Turn, QStringLiteral("m"), QString());
            ContentionLedger::markDispatched(id);
            ContentionLedger::end(id, true);
        }
        QVERIFY(worker->wait(10000));
        delete worker;
        QVERIFY(ok.load());
        QCOMPARE(ContentionLedger::openCount(), 0);
    }
};

QTEST_MAIN(TestContentionLedger)
#include "test-contention-ledger.moc"
