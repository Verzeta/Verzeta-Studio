// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: GPL-3.0-or-later


#include "../../backend/inference/infer-protocol.h"

#include <QtTest>

#include <QProcess>
#ifdef VERZETA_HAS_LLAMACPP
#include "../../backend/workers/embedding-worker.h"
#endif

#include <cmath>

using Verzeta::Infer::encodeInferFrame;
using Verzeta::Infer::InferFrame;
using Verzeta::Infer::InferFrameReader;
using Verzeta::Infer::kInferProtocolVersion;

class TestInferenceSidecar : public QObject {
    Q_OBJECT

  private:
    QProcess m_proc;
    InferFrameReader m_reader;
    QList<InferFrame> m_frames;

    bool waitForFrames(int count, int timeoutMs = 8000) {
        QElapsedTimer t;
        t.start();
        while (m_frames.size() < count && t.elapsed() < timeoutMs) {
            if (!m_proc.waitForReadyRead(200))
                continue;
            const bool ok = m_reader.feed(m_proc.readAllStandardOutput(),
                                          [this](const InferFrame& f) { m_frames.append(f); });
            if (!ok)
                return false;
        }
        return m_frames.size() >= count;
    }

    void sendFrame(const QJsonObject& header) {
        const QByteArray bytes = encodeInferFrame(header);
        QVERIFY(!bytes.isEmpty());
        QCOMPARE(m_proc.write(bytes), qint64(bytes.size()));
        QVERIFY(m_proc.waitForBytesWritten(2000));
    }

  private slots:
    void init() {
        m_frames.clear();
        m_reader.clear();
        m_proc.setProgram(QStringLiteral(VERZETA_INFERENCE_BIN));
        m_proc.setProcessChannelMode(QProcess::SeparateChannels);
        m_proc.start();
        QVERIFY2(m_proc.waitForStarted(5000), "verzeta-inference failed to start");
        QVERIFY2(waitForFrames(1), "no hello frame from sidecar");
        QCOMPARE(m_frames.first().header.value(QStringLiteral("op")).toString(),
                 QStringLiteral("hello"));
        QCOMPARE(m_frames.first().header.value(QStringLiteral("proto")).toInt(),
                 kInferProtocolVersion);
        m_frames.clear();
    }

    void cleanup() {
        if (m_proc.state() != QProcess::NotRunning) {
            m_proc.closeWriteChannel();
            if (!m_proc.waitForFinished(3000)) {
                m_proc.kill();
                m_proc.waitForFinished(1000);
            }
        }
    }

    void test_ping_pong_echoesId() {
        QJsonObject ping;
        ping.insert(QStringLiteral("op"), QStringLiteral("ping"));
        ping.insert(QStringLiteral("id"), 7);
        sendFrame(ping);
        QVERIFY(waitForFrames(1));
        QCOMPARE(m_frames.first().header.value(QStringLiteral("op")).toString(),
                 QStringLiteral("pong"));
        QCOMPARE(m_frames.first().header.value(QStringLiteral("id")).toInt(), 7);
    }

    void test_status_reportsProtoAndUnloadedSlots() {
        QJsonObject status;
        status.insert(QStringLiteral("op"), QStringLiteral("status"));
        status.insert(QStringLiteral("id"), 1);
        sendFrame(status);
        QVERIFY(waitForFrames(1));
        const QJsonObject& h = m_frames.first().header;
        QCOMPARE(h.value(QStringLiteral("op")).toString(), QStringLiteral("status_result"));
        QVERIFY(h.value(QStringLiteral("ok")).toBool());
        QCOMPARE(h.value(QStringLiteral("proto")).toInt(), kInferProtocolVersion);
        const QJsonObject slotMap = h.value(QStringLiteral("slots")).toObject();
        QVERIFY(slotMap.contains(QStringLiteral("embed")));
        QCOMPARE(slotMap.value(QStringLiteral("embed"))
                     .toObject()
                     .value(QStringLiteral("loaded"))
                     .toBool(true),
                 false);
    }

    void test_modelOps_structuredErrors_whenNotLoaded() {
        for (const char* op : {"embed", "complete"}) {
            m_frames.clear();
            QJsonObject req;
            req.insert(QStringLiteral("op"), QLatin1String(op));
            req.insert(QStringLiteral("id"), 9);
            if (qstrcmp(op, "embed") == 0) {
                req.insert(QStringLiteral("texts"), QJsonArray{QStringLiteral("x")});
            }
            sendFrame(req);
            QVERIFY2(waitForFrames(1), op);
            const QJsonObject& h = m_frames.first().header;
            QCOMPARE(h.value(QStringLiteral("op")).toString(), QStringLiteral("error"));
            QVERIFY(
                h.value(QStringLiteral("error")).toString().contains(QStringLiteral("not loaded")));
            QCOMPARE(h.value(QStringLiteral("id")).toInt(), 9);
        }

        m_frames.clear();
        QJsonObject load;
        load.insert(QStringLiteral("op"), QStringLiteral("load_model"));
        load.insert(QStringLiteral("slot"), QStringLiteral("embed"));
        load.insert(QStringLiteral("path"), QStringLiteral("/nonexistent/model.gguf"));
        load.insert(QStringLiteral("id"), 10);
        sendFrame(load);
        QVERIFY(waitForFrames(1));
        QCOMPARE(m_frames.first().header.value(QStringLiteral("op")).toString(),
                 QStringLiteral("error"));

        m_frames.clear();
        QJsonObject badSlot;
        badSlot.insert(QStringLiteral("op"), QStringLiteral("load_model"));
        badSlot.insert(QStringLiteral("slot"), QStringLiteral("nope"));
        badSlot.insert(QStringLiteral("id"), 11);
        sendFrame(badSlot);
        QVERIFY(waitForFrames(1));
        QVERIFY(m_frames.first()
                    .header.value(QStringLiteral("error"))
                    .toString()
                    .contains(QStringLiteral("unsupported slot")));

        m_frames.clear();
        QJsonObject unload;
        unload.insert(QStringLiteral("op"), QStringLiteral("unload"));
        unload.insert(QStringLiteral("slot"), QStringLiteral("embed"));
        sendFrame(unload);
        QVERIFY(waitForFrames(1));
        QCOMPARE(m_frames.first().header.value(QStringLiteral("op")).toString(),
                 QStringLiteral("unload_result"));
        QVERIFY(m_frames.first().header.value(QStringLiteral("ok")).toBool());
    }

    void test_realModel_embed_andParityWithInProcess() {
        const QString gguf = qEnvironmentVariable("VERZETA_TEST_EMBED_GGUF");
        if (gguf.isEmpty() || !QFile::exists(gguf)) {
            QSKIP("VERZETA_TEST_EMBED_GGUF not set — skipping "
                  "real-model parity test");
        }

        QJsonObject load;
        load.insert(QStringLiteral("op"), QStringLiteral("load_model"));
        load.insert(QStringLiteral("slot"), QStringLiteral("embed"));
        load.insert(QStringLiteral("path"), gguf);
        load.insert(QStringLiteral("id"), 1);
        sendFrame(load);
        QVERIFY(waitForFrames(1, 60000));
        const QJsonObject loadRes = m_frames.first().header;
        QVERIFY2(loadRes.value(QStringLiteral("ok")).toBool(),
                 qPrintable(loadRes.value(QStringLiteral("error")).toString()));
        const int dim = loadRes.value(QStringLiteral("dim")).toInt();
        QVERIFY(dim > 0);
        const QString accel = loadRes.value(QStringLiteral("accel")).toString();
        QVERIFY2(accel == QStringLiteral("cpu") || accel.startsWith(QStringLiteral("gpu (")),
                 qPrintable(accel));

        m_frames.clear();
        const QString probe = QStringLiteral("the quick brown fox");
        QJsonObject embed;
        embed.insert(QStringLiteral("op"), QStringLiteral("embed"));
        embed.insert(QStringLiteral("id"), 2);
        embed.insert(QStringLiteral("texts"), QJsonArray{probe, QStringLiteral("hello world")});
        sendFrame(embed);
        QVERIFY(waitForFrames(1, 60000));
        const InferFrame result = m_frames.first();
        QVERIFY2(result.header.value(QStringLiteral("ok")).toBool(),
                 qPrintable(result.header.value(QStringLiteral("error")).toString()));
        QCOMPARE(result.header.value(QStringLiteral("count")).toInt(), 2);
        QCOMPARE(result.header.value(QStringLiteral("dim")).toInt(), dim);
        QCOMPARE(result.blob.size(), static_cast<qsizetype>(2 * dim * sizeof(float)));

#ifdef VERZETA_HAS_LLAMACPP
        EmbeddingWorker worker;
        worker.setBackend(EmbeddingWorker::EmbeddingBackend::LlamaCpp);
        worker.setModelPath(gguf);
        QSignalSpy ready(&worker, &EmbeddingWorker::embeddingReady);
        worker.computeEmbedding(QStringLiteral("p1"), probe, QStringLiteral("message"));
        QCOMPARE(ready.count(), 1);
        const QVector<float> inProc = ready.first().at(1).value<QVector<float>>();
        QCOMPARE(inProc.size(), dim);

        const float* side = reinterpret_cast<const float*>(result.blob.constData());
        double dot = 0.0, a = 0.0, b = 0.0;
        for (int i = 0; i < dim; ++i) {
            dot += static_cast<double>(side[i]) * inProc[i];
            a += static_cast<double>(side[i]) * side[i];
            b += static_cast<double>(inProc[i]) * inProc[i];
        }
        const double cosine = dot / (std::sqrt(a) * std::sqrt(b));
        QVERIFY2(cosine >= 0.9999, qPrintable(QStringLiteral("cosine=%1").arg(cosine)));
#endif

        m_frames.clear();
        QJsonObject status;
        status.insert(QStringLiteral("op"), QStringLiteral("status"));
        sendFrame(status);
        QVERIFY(waitForFrames(1));
        QVERIFY(m_frames.first()
                    .header.value(QStringLiteral("slots"))
                    .toObject()
                    .value(QStringLiteral("embed"))
                    .toObject()
                    .value(QStringLiteral("loaded"))
                    .toBool());
    }

    void test_unsupportedOp_structuredError_streamSurvives() {
        QJsonObject bogus;
        bogus.insert(QStringLiteral("op"), QStringLiteral("dance"));
        bogus.insert(QStringLiteral("id"), 3);
        sendFrame(bogus);
        QVERIFY(waitForFrames(1));
        QVERIFY(m_frames.first()
                    .header.value(QStringLiteral("error"))
                    .toString()
                    .contains(QStringLiteral("unsupported")));
        m_frames.clear();
        QJsonObject ping;
        ping.insert(QStringLiteral("op"), QStringLiteral("ping"));
        sendFrame(ping);
        QVERIFY(waitForFrames(1));
        QCOMPARE(m_frames.first().header.value(QStringLiteral("op")).toString(),
                 QStringLiteral("pong"));
    }

    void test_realModel_ragpComplete() {
        const QString gguf = qEnvironmentVariable("VERZETA_TEST_RAGP_GGUF");
        if (gguf.isEmpty() || !QFile::exists(gguf)) {
            QSKIP("VERZETA_TEST_RAGP_GGUF not set — skipping "
                  "real-model ragp test");
        }

        QJsonObject load;
        load.insert(QStringLiteral("op"), QStringLiteral("load_model"));
        load.insert(QStringLiteral("slot"), QStringLiteral("ragp"));
        load.insert(QStringLiteral("path"), gguf);
        load.insert(QStringLiteral("id"), 1);
        sendFrame(load);
        QVERIFY(waitForFrames(1, 120000));
        const QJsonObject loadRes = m_frames.first().header;
        QVERIFY2(loadRes.value(QStringLiteral("ok")).toBool(),
                 qPrintable(loadRes.value(QStringLiteral("error")).toString()));

        m_frames.clear();
        QJsonObject req;
        req.insert(QStringLiteral("op"), QStringLiteral("complete"));
        req.insert(QStringLiteral("id"), 2);
        req.insert(QStringLiteral("prompt"),
                   QStringLiteral("Answer with exactly one word, YES or"
                                  " NO: is the sky above the ground?"));
        req.insert(QStringLiteral("max_tokens"), 8);
        req.insert(QStringLiteral("chat_template"), true);
        sendFrame(req);
        QVERIFY(waitForFrames(1, 120000));
        const QJsonObject res = m_frames.first().header;
        QVERIFY2(res.value(QStringLiteral("ok")).toBool(),
                 qPrintable(res.value(QStringLiteral("error")).toString()));
        const QString text = res.value(QStringLiteral("text")).toString().trimmed();
        QVERIFY2(!text.isEmpty(), "empty completion");

        m_frames.clear();
        QJsonObject jsonReq;
        jsonReq.insert(QStringLiteral("op"), QStringLiteral("complete"));
        jsonReq.insert(QStringLiteral("id"), 3);
        jsonReq.insert(QStringLiteral("prompt"),
                       QStringLiteral("Reply with ONLY this exact JSON "
                                      "object and nothing else: "
                                      "{\"ok\":true}"));
        jsonReq.insert(QStringLiteral("max_tokens"), 128);
        jsonReq.insert(QStringLiteral("json_stop"), true);
        jsonReq.insert(QStringLiteral("chat_template"), true);
        sendFrame(jsonReq);
        QVERIFY(waitForFrames(1, 120000));
        const QString jsonText = m_frames.first().header.value(QStringLiteral("text")).toString();
        const int open = jsonText.count(QLatin1Char('{'));
        const int close = jsonText.count(QLatin1Char('}'));
        QVERIFY2(open > 0 && open == close,
                 qPrintable(QStringLiteral("unbalanced: %1").arg(jsonText)));
    }

    void test_stdinEof_cleanExitZero() {
        m_proc.closeWriteChannel();
        QVERIFY2(m_proc.waitForFinished(5000), "sidecar did not exit on stdin EOF");
        QCOMPARE(m_proc.exitStatus(), QProcess::NormalExit);
        QCOMPARE(m_proc.exitCode(), 0);
    }
};

QTEST_MAIN(TestInferenceSidecar)
#include "test-inference-sidecar.moc"
