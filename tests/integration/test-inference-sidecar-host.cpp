// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: GPL-3.0-or-later


#include "../../backend/services/chat/action-intent-confirmer.h"
#include "../../backend/services/inference-sidecar-host.h"

#include <QThread>
#include <QtTest>

using Verzeta::Infer::InferenceSidecarHost;

class TestInferenceSidecarHost : public QObject {
    Q_OBJECT

  private:
    template <typename Fn> void onWorker(Fn fn) {
        QThread* t = QThread::create(fn);
        t->start();
        QVERIFY(t->wait(180000));
        delete t;
    }

  private slots:
    void test_completeWithoutModel_structuredError() {
        InferenceSidecarHost host;
        host.setBinaryPathOverride(QStringLiteral(VERZETA_INFERENCE_BIN));
        QString err;
        QString text;
        onWorker([&]() {
            text = host.completeBlocking(QStringLiteral("hi"), QString(), 8, false, &err);
        });
        QVERIFY(text.isNull());
        QVERIFY2(err.contains(QStringLiteral("not loaded")), qPrintable(err));
        QVERIFY(host.isAvailable());
    }

    void test_missingBinary_latchesUnavailable() {
        InferenceSidecarHost host;
        host.setBinaryPathOverride(QStringLiteral("/nonexistent/verzeta-inference"));
        QString err;
        onWorker([&]() {
            for (int i = 0; i < 3; ++i) {
                err.clear();
                (void)host.embedBlocking(QStringLiteral("x"), QString(), nullptr, &err, 5000);
            }
        });
        QVERIFY2(err.contains(QStringLiteral("not found")) ||
                     err.contains(QStringLiteral("unavailable")),
                 qPrintable(err));
        QTRY_VERIFY(!host.isAvailable());
    }

    void test_destructor_reapsLiveChild_noAbort() {
        qint64 pid = 0;
        {
            InferenceSidecarHost host;
            host.setBinaryPathOverride(QStringLiteral(VERZETA_INFERENCE_BIN));
            QString err;
            onWorker([&]() {
                (void)host.completeBlocking(QStringLiteral("hi"), QString(), 4, false, &err, 30000);
            });
        }
        Q_UNUSED(pid);
        QVERIFY(true);
    }

    void test_realModels_embedAndComplete() {
        const QString embedGguf = qEnvironmentVariable("VERZETA_TEST_EMBED_GGUF");
        const QString ragpGguf = qEnvironmentVariable("VERZETA_TEST_RAGP_GGUF");
        if (embedGguf.isEmpty() || !QFile::exists(embedGguf)) {
            QSKIP("VERZETA_TEST_EMBED_GGUF not set");
        }
        InferenceSidecarHost host;
        host.setBinaryPathOverride(QStringLiteral(VERZETA_INFERENCE_BIN));

        QVector<float> vec;
        QString modelId;
        QString err;
        onWorker([&]() {
            vec = host.embedBlocking(
                QStringLiteral("the quick brown fox"), embedGguf, &modelId, &err);
        });
        QVERIFY2(!vec.isEmpty(), qPrintable(err));
        QCOMPARE(modelId, QFileInfo(embedGguf).fileName());

        QVector<float> vec2;
        onWorker([&]() {
            vec2 = host.embedBlocking(
                QStringLiteral("the quick brown fox"), embedGguf, nullptr, &err, 30000);
        });
        QCOMPARE(vec2, vec);

        if (ragpGguf.isEmpty() || !QFile::exists(ragpGguf)) {
            QSKIP("VERZETA_TEST_RAGP_GGUF not set — embed half done");
        }
        QString text;
        onWorker([&]() {
            text = host.completeBlocking(QStringLiteral("<|user|>Answer with exactly one word,"
                                                        " YES or NO: is water wet?<|end|>"
                                                        "<|assistant|>"),
                                         ragpGguf,
                                         8,
                                         false,
                                         &err);
        });
        QVERIFY2(!text.isNull(), qPrintable(err));
        QVERIFY(!text.trimmed().isEmpty());
    }

    void test_realModel_confirmerStanceJudgements() {
        const QString gguf = qEnvironmentVariable("VERZETA_TEST_RAGP_GGUF");
        if (gguf.isEmpty() || !QFile::exists(gguf)) {
            QSKIP("VERZETA_TEST_RAGP_GGUF not set");
        }
        InferenceSidecarHost host;
        host.setBinaryPathOverride(QStringLiteral(VERZETA_INFERENCE_BIN));

        using Chat::ActionIntentConfirmer;
        struct Case {
            QString reply;
            ActionIntentConfirmer::Result want;
            QString name;
        };
        const QVector<Case> cases = {
            {QStringLiteral("I have finished generating and executing the edit on "
                            "Sales_One_Pager.md within the canvas. The document is now "
                            "significantly richer. Project \"Product Launch Plan 2\" is "
                            "officially complete. Do you have any final questions, or "
                            "can we consider this project phase closed?"),
             ActionIntentConfirmer::Result::Rejected,
             QStringLiteral("wrapup+question")},
            {QStringLiteral("I apologize for the repeated execution failures on "
                            "complete_task. Since I cannot execute the tool call "
                            "cleanly right now, I will make a final verbal declaration "
                            "instead. Is there anything else I can assist with?"),
             ActionIntentConfirmer::Result::Rejected,
             QStringLiteral("apology+decline")},
            {QStringLiteral("Would it be best if we create a new file named "
                            "V2_Receipt_Design_Spec.md? Let me know what feels right."),
             ActionIntentConfirmer::Result::Rejected,
             QStringLiteral("permission-ask")},
            {QStringLiteral("Understood. I'll create pricing.md now using write_file."),
             ActionIntentConfirmer::Result::Confirmed,
             QStringLiteral("commitment")},
        };
        for (const Case& c : cases) {
            const QString prompt = ActionIntentConfirmer::buildIntentPrompt(c.reply, QString());
            QString text, err;
            onWorker([&]() { text = host.completeBlocking(prompt, gguf, 8, true, &err); });
            QVERIFY2(!text.isNull(), qPrintable(err));
            const auto got = ActionIntentConfirmer::parseIntentAnswer(text);
            QVERIFY2(got == c.want,
                     qPrintable(QStringLiteral("case '%1': model said '%2' (parsed %3), wanted %4")
                                    .arg(c.name, text.trimmed())
                                    .arg(int(got))
                                    .arg(int(c.want))));
        }
    }

    void test_realModel_chatStream_andCancel() {
        const QString gguf = qEnvironmentVariable("VERZETA_TEST_RAGP_GGUF");
        if (gguf.isEmpty() || !QFile::exists(gguf)) {
            QSKIP("VERZETA_TEST_RAGP_GGUF not set");
        }
        InferenceSidecarHost host;
        host.setBinaryPathOverride(QStringLiteral(VERZETA_INFERENCE_BIN));
        QSignalSpy chunks(&host, &InferenceSidecarHost::chatStreamChunk);
        QSignalSpy finished(&host, &InferenceSidecarHost::chatStreamFinished);

        const quint64 id =
            host.startChatStream(QStringLiteral("<|im_start|>user\nCount from 1 to 30 as"
                                                " words.<|im_end|>\n<|im_start|>assistant\n"),
                                 gguf,
                                 4096,
                                 24,
                                 0.0);
        QVERIFY(id != 0);
        QVERIFY2(finished.wait(180000), "stream never finished");
        QCOMPARE(finished.count(), 1);
        const auto fin = finished.takeFirst();
        QCOMPARE(fin.at(0).toULongLong(), id);
        QVERIFY2(fin.at(1).toBool(), qPrintable(fin.at(4).toString()));
        QCOMPARE(fin.at(2).toString(), QStringLiteral("length"));
        QVERIFY(chunks.count() > 0);
        for (const auto& c : chunks) {
            QCOMPARE(c.at(0).toULongLong(), id);
        }

        chunks.clear();
        QSignalSpy finished2(&host, &InferenceSidecarHost::chatStreamFinished);
        const quint64 id2 =
            host.startChatStream(QStringLiteral("<|im_start|>user\nWrite a very long story"
                                                ".<|im_end|>\n<|im_start|>assistant\n"),
                                 gguf,
                                 4096,
                                 8192,
                                 0.0);
        QVERIFY(id2 != 0);
        QTRY_VERIFY_WITH_TIMEOUT(chunks.count() > 0, 120000);
        host.cancelChatStream(id2);
        QVERIFY2(finished2.wait(30000), "cancel did not terminate the stream");
        const auto fin2 = finished2.takeFirst();
        QCOMPARE(fin2.at(0).toULongLong(), id2);
        QVERIFY(fin2.at(1).toBool());
        QCOMPARE(fin2.at(2).toString(), QStringLiteral("cancelled"));
    }
};

QTEST_MAIN(TestInferenceSidecarHost)
#include "test-inference-sidecar-host.moc"
