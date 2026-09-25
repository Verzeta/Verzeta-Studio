// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: GPL-3.0-or-later


#include "../../backend/inference/infer-protocol.h"

#include <QtTest>

using Verzeta::Infer::decodeInferPayload;
using Verzeta::Infer::encodeInferFrame;
using Verzeta::Infer::InferFrame;
using Verzeta::Infer::InferFrameReader;
using Verzeta::Infer::kMaxInferFrameBytes;

class TestInferProtocol : public QObject {
    Q_OBJECT

  private slots:
    void test_roundTrip_headerOnly() {
        QJsonObject h;
        h.insert(QStringLiteral("op"), QStringLiteral("ping"));
        h.insert(QStringLiteral("id"), 42);
        const QByteArray frame = encodeInferFrame(h);
        QVERIFY(frame.size() > 8);

        InferFrame decoded;
        QVERIFY(decodeInferPayload(frame.mid(4), &decoded));
        QCOMPARE(decoded.header.value(QStringLiteral("op")).toString(), QStringLiteral("ping"));
        QCOMPARE(decoded.header.value(QStringLiteral("id")).toInt(), 42);
        QVERIFY(decoded.blob.isEmpty());
    }

    void test_roundTrip_float32Blob_byteExact() {
        const QVector<float> vec = {0.25f, -1.5f, 3.14159f, 0.0f};
        QByteArray blob(reinterpret_cast<const char*>(vec.constData()),
                        static_cast<qsizetype>(vec.size() * sizeof(float)));
        QJsonObject h;
        h.insert(QStringLiteral("op"), QStringLiteral("embed_result"));
        h.insert(QStringLiteral("dim"), 4);
        const QByteArray frame = encodeInferFrame(h, blob);

        InferFrame decoded;
        QVERIFY(decodeInferPayload(frame.mid(4), &decoded));
        QCOMPARE(decoded.blob, blob);
        const float* back = reinterpret_cast<const float*>(decoded.blob.constData());
        QCOMPARE(back[2], 3.14159f);
    }

    void test_reader_partialFeed_and_multiFrame() {
        QJsonObject a;
        a.insert(QStringLiteral("op"), QStringLiteral("a"));
        QJsonObject b;
        b.insert(QStringLiteral("op"), QStringLiteral("b"));
        const QByteArray stream =
            encodeInferFrame(a, QByteArrayLiteral("blobA")) + encodeInferFrame(b);

        InferFrameReader reader;
        QStringList ops;
        for (qsizetype i = 0; i < stream.size(); ++i) {
            QVERIFY(reader.feed(stream.mid(i, 1), [&](const InferFrame& f) {
                ops << f.header.value(QStringLiteral("op")).toString();
            }));
        }
        QCOMPARE(ops, (QStringList{QStringLiteral("a"), QStringLiteral("b")}));

        InferFrameReader reader2;
        int count = 0;
        QVERIFY(reader2.feed(stream, [&](const InferFrame& f) {
            ++count;
            if (count == 1) {
                QCOMPARE(f.blob, QByteArrayLiteral("blobA"));
            }
        }));
        QCOMPARE(count, 2);
    }

    void test_oversized_refusals() {
        QJsonObject h;
        h.insert(QStringLiteral("op"), QStringLiteral("x"));
        QByteArray huge(static_cast<qsizetype>(kMaxInferFrameBytes), 'x');
        QVERIFY(encodeInferFrame(h, huge).isEmpty());

        QByteArray bad;
        const quint32 lie = static_cast<quint32>(kMaxInferFrameBytes) + 1;
        bad.append(static_cast<char>((lie >> 24) & 0xff));
        bad.append(static_cast<char>((lie >> 16) & 0xff));
        bad.append(static_cast<char>((lie >> 8) & 0xff));
        bad.append(static_cast<char>(lie & 0xff));
        InferFrameReader reader;
        QVERIFY(!reader.feed(bad, [](const InferFrame&) {}));
    }

    void test_malformedPayload_skipped_streamStaysAligned() {
        QByteArray garbagePayload;
        garbagePayload.append(char(0)).append(char(0)).append(char(0)).append(char(7));
        garbagePayload.append("not-json");
        QByteArray stream;
        const quint32 len = static_cast<quint32>(garbagePayload.size());
        stream.append(static_cast<char>((len >> 24) & 0xff));
        stream.append(static_cast<char>((len >> 16) & 0xff));
        stream.append(static_cast<char>((len >> 8) & 0xff));
        stream.append(static_cast<char>(len & 0xff));
        stream.append(garbagePayload);
        QJsonObject good;
        good.insert(QStringLiteral("op"), QStringLiteral("after"));
        stream += encodeInferFrame(good);

        InferFrameReader reader;
        QStringList ops;
        QVERIFY(reader.feed(stream, [&](const InferFrame& f) {
            ops << f.header.value(QStringLiteral("op")).toString();
        }));
        QCOMPARE(ops, QStringList{QStringLiteral("after")});
    }

    void test_decode_truncatedPayload_fails() {
        InferFrame out;
        QVERIFY(!decodeInferPayload(QByteArrayLiteral("ab"), &out));
        QByteArray lying;
        lying.append(char(0)).append(char(0)).append(char(0)).append(char(50));
        lying.append("{}");
        QVERIFY(!decodeInferPayload(lying, &out));
        QVERIFY(!decodeInferPayload(QByteArray(), nullptr));
    }
};

QTEST_MAIN(TestInferProtocol)
#include "test-infer-protocol.moc"
