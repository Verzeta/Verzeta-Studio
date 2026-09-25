// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/api/llm-interface.h"
#include "../../backend/services/chat/request-builder.h"

#include <QtTest>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

LlmMessage writeAsst(const QString& callId, const QString& file, const QString& body) {
    LlmMessage m;
    m.role = QStringLiteral("assistant");
    QJsonObject fn;
    fn[QStringLiteral("name")] = QStringLiteral("write_file");
    fn[QStringLiteral("arguments")] =
        QJsonObject{{QStringLiteral("filename"), file}, {QStringLiteral("content"), body}};
    QJsonObject tc;
    tc[QStringLiteral("id")] = callId;
    tc[QStringLiteral("type")] = QStringLiteral("function");
    tc[QStringLiteral("function")] = fn;
    m.toolCallsJson = QJsonArray{tc};
    return m;
}

LlmMessage toolResult(const QString& callId, const QString& content) {
    LlmMessage m;
    m.role = QStringLiteral("tool");
    m.toolCallId = callId;
    m.content = content;
    return m;
}

LlmMessage userMsg(const QString& text) {
    LlmMessage m;
    m.role = QStringLiteral("user");
    m.content = text;
    return m;
}

LlmMessage assistantPlain(const QString& text) {
    LlmMessage m;
    m.role = QStringLiteral("assistant");
    m.content = text;
    return m;
}

bool isCollapsedRecord(const LlmMessage& m) {
    return m.role == QStringLiteral("system") &&
           m.content.startsWith(QStringLiteral("[earlier tool activity"));
}

int collapsedRecordCount(const QList<LlmMessage>& msgs) {
    int n = 0;
    for (const LlmMessage& m : msgs) {
        if (isCollapsedRecord(m))
            ++n;
    }
    return n;
}

int verbatimToolGroups(const QList<LlmMessage>& msgs) {
    int n = 0;
    for (const LlmMessage& m : msgs) {
        if (m.role == QStringLiteral("assistant") && !m.toolCallsJson.isEmpty())
            ++n;
    }
    return n;
}

bool anyContains(const QList<LlmMessage>& msgs, const QString& needle) {
    for (const LlmMessage& m : msgs) {
        if (m.content.contains(needle))
            return true;
        if (!m.toolCallsJson.isEmpty()) {
            const QString s =
                QString::fromUtf8(QJsonDocument(m.toolCallsJson).toJson(QJsonDocument::Compact));
            if (s.contains(needle))
                return true;
        }
    }
    return false;
}

bool noBodylessContentCall(const QList<LlmMessage>& msgs) {
    for (const LlmMessage& m : msgs) {
        for (const QJsonValue& v : m.toolCallsJson) {
            const QJsonObject fn = v.toObject().value(QStringLiteral("function")).toObject();
            const QString name = fn.value(QStringLiteral("name")).toString();
            if (name != QStringLiteral("write_file") && name != QStringLiteral("open_canvas") &&
                name != QStringLiteral("edit_canvas")) {
                continue;
            }
            const QJsonObject args = fn.value(QStringLiteral("arguments")).toObject();
            const bool hasBody = !args.value(QStringLiteral("content")).toString().isEmpty() ||
                                 !args.value(QStringLiteral("new_content")).toString().isEmpty();
            if (!hasBody)
                return false;
        }
    }
    return true;
}

}  // namespace

class TestHistoryToolDigest : public QObject {
    Q_OBJECT

  private slots:
    void test_newestGroupsProtected_oldestDigestedByShare() {
        const QString smallBody = QString(1500, QLatin1Char('a'));
        QList<LlmMessage> msgs;
        for (int i = 0; i < 6; ++i) {
            const QString id = QStringLiteral("c%1").arg(i);
            msgs.append(writeAsst(
                id, QStringLiteral("f%1.txt").arg(i), smallBody + QStringLiteral("/%1").arg(i)));
            msgs.append(toolResult(id, QStringLiteral("{\"ok\":true}")));
        }
        msgs.append(userMsg(QStringLiteral("continue")));

        Chat::RequestBuilder::digestToolPayloads(msgs, 4000);

        QVERIFY2(!anyContains(msgs, QStringLiteral("/0")),
                 "oldest write body must be digested away");
        QVERIFY2(anyContains(msgs, QStringLiteral("/5")), "newest write body must stay verbatim");
        QCOMPARE(collapsedRecordCount(msgs), 1);
        QCOMPARE(verbatimToolGroups(msgs), 5);
        QVERIFY2(!anyContains(msgs, QStringLiteral("__digest")),
                 "the __digest sentinel must never reach the model");
        QVERIFY(noBodylessContentCall(msgs));
    }

    void test_hugeNewestWriteDigestedByCeiling() {
        const QString hugeBody = QString(4000, QLatin1Char('x'));
        QList<LlmMessage> msgs;
        msgs.append(writeAsst(QStringLiteral("c0"), QStringLiteral("big.html"), hugeBody));
        msgs.append(toolResult(QStringLiteral("c0"), QStringLiteral("{\"ok\":true}")));
        msgs.append(userMsg(QStringLiteral("go")));

        Chat::RequestBuilder::digestToolPayloads(msgs, 32768);

        QCOMPARE(collapsedRecordCount(msgs), 1);
        QCOMPARE(verbatimToolGroups(msgs), 0);
        QVERIFY(!anyContains(msgs, hugeBody));
        QVERIFY(!anyContains(msgs, QStringLiteral("__digest")));
        QVERIFY(noBodylessContentCall(msgs));
        QVERIFY(anyContains(msgs, QStringLiteral("big.html")));
    }

    void test_protectedLargeReadResult_staysVerbatim() {
        const QString canvasDoc = QStringLiteral("{\"content\":\"CANVAS-DOC-MARKER ") +
                                  QString(5000, QLatin1Char('m')) + QStringLiteral("\"}");
        QList<LlmMessage> msgs;
        LlmMessage readCall;
        readCall.role = QStringLiteral("assistant");
        QJsonObject fn;
        fn[QStringLiteral("name")] = QStringLiteral("read_canvas");
        fn[QStringLiteral("arguments")] = QJsonObject{};
        QJsonObject tc;
        tc[QStringLiteral("id")] = QStringLiteral("r0");
        tc[QStringLiteral("type")] = QStringLiteral("function");
        tc[QStringLiteral("function")] = fn;
        readCall.toolCallsJson = QJsonArray{tc};
        msgs.append(readCall);
        msgs.append(toolResult(QStringLiteral("r0"), canvasDoc));
        msgs.append(userMsg(QStringLiteral("continue")));

        Chat::RequestBuilder::digestToolPayloads(msgs, 32768);

        QCOMPARE(collapsedRecordCount(msgs), 0);
        QVERIFY2(anyContains(msgs, QStringLiteral("CANVAS-DOC-MARKER")),
                 "a fresh read result in the protected window must stay "
                 "visible to the model that fetched it");
        QCOMPARE(verbatimToolGroups(msgs), 1);
    }

    void test_protectedMonsterResult_stillDigested() {
        const QString monster = QStringLiteral("{\"content\":\"") +
                                QString(12000, QLatin1Char('n')) + QStringLiteral("\"}");
        QList<LlmMessage> msgs;
        LlmMessage readCall;
        readCall.role = QStringLiteral("assistant");
        QJsonObject fn;
        fn[QStringLiteral("name")] = QStringLiteral("read_canvas");
        fn[QStringLiteral("arguments")] = QJsonObject{};
        QJsonObject tc;
        tc[QStringLiteral("id")] = QStringLiteral("r1");
        tc[QStringLiteral("type")] = QStringLiteral("function");
        tc[QStringLiteral("function")] = fn;
        readCall.toolCallsJson = QJsonArray{tc};
        msgs.append(readCall);
        msgs.append(toolResult(QStringLiteral("r1"), monster));
        msgs.append(userMsg(QStringLiteral("go")));

        Chat::RequestBuilder::digestToolPayloads(msgs, 32768);

        QCOMPARE(collapsedRecordCount(msgs), 1);
        QCOMPARE(verbatimToolGroups(msgs), 0);
    }

    void test_byteIdenticalDuplicateDeduped() {
        const QString body = QString(600, QLatin1Char('z'));
        QList<LlmMessage> msgs;
        msgs.append(writeAsst(QStringLiteral("c0"), QStringLiteral("dup.txt"), body));
        msgs.append(toolResult(QStringLiteral("c0"), QStringLiteral("{\"ok\":true}")));
        msgs.append(writeAsst(QStringLiteral("c1"), QStringLiteral("dup.txt"), body));
        msgs.append(toolResult(QStringLiteral("c1"), QStringLiteral("{\"ok\":true}")));
        msgs.append(userMsg(QStringLiteral("done")));

        Chat::RequestBuilder::digestToolPayloads(msgs, 32768);

        QCOMPARE(collapsedRecordCount(msgs), 1);
        QCOMPARE(verbatimToolGroups(msgs), 1);
        QVERIFY(anyContains(msgs, body));
        QVERIFY(!anyContains(msgs, QStringLiteral("__digest")));
        QVERIFY(noBodylessContentCall(msgs));
    }

    void test_digestedGroupCollapsesWithNoOrphanToolRow() {
        const QString hugeBody = QString(4000, QLatin1Char('q'));
        QList<LlmMessage> msgs;
        msgs.append(writeAsst(QStringLiteral("call_old"), QStringLiteral("old.txt"), hugeBody));
        msgs.append(toolResult(QStringLiteral("call_old"), QStringLiteral("{\"ok\":true}")));
        for (int i = 0; i < 3; ++i) {
            const QString id = QStringLiteral("n%1").arg(i);
            msgs.append(
                writeAsst(id, QStringLiteral("n%1.txt").arg(i), QString(300, QLatin1Char('a'))));
            msgs.append(toolResult(id, QStringLiteral("{\"ok\":true}")));
        }
        msgs.append(userMsg(QStringLiteral("x")));

        Chat::RequestBuilder::digestToolPayloads(msgs, 32768);

        for (const LlmMessage& m : msgs) {
            QVERIFY2(
                !(m.role == QStringLiteral("tool") && m.toolCallId == QStringLiteral("call_old")),
                "digested group's tool row must be absorbed, not orphaned");
        }
        QCOMPARE(collapsedRecordCount(msgs), 1);
        bool keptPairFound = false;
        for (int i = 0; i + 1 < msgs.size(); ++i) {
            if (msgs.at(i).role == QStringLiteral("assistant") &&
                !msgs.at(i).toolCallsJson.isEmpty() &&
                msgs.at(i + 1).role == QStringLiteral("tool")) {
                keptPairFound = true;
                break;
            }
        }
        QVERIFY2(keptPairFound, "protected groups keep walk-and-pair structure");
        QVERIFY(!anyContains(msgs, QStringLiteral("__digest")));
        QVERIFY(noBodylessContentCall(msgs));
    }

    void test_noToolGroups_noChange() {
        QList<LlmMessage> msgs;
        msgs.append(userMsg(QStringLiteral("hi")));
        LlmMessage a;
        a.role = QStringLiteral("assistant");
        a.content = QStringLiteral("hello");
        msgs.append(a);
        const QList<LlmMessage> before = msgs;
        Chat::RequestBuilder::digestToolPayloads(msgs, 8192);
        QCOMPARE(msgs.size(), before.size());
        QCOMPARE(msgs.at(1).content, QStringLiteral("hello"));
    }

    void test_shape_growsToGuaranteeOutputRoom() {
        LlmRequest req;
        req.config.contextWindow = 4096;
        req.messages.append(userMsg(QString(6000, QLatin1Char('a'))));
        const int fill = Chat::RequestBuilder::shapeRequestToWindow(req, 500, 500, true);
        const int floor = 3072;
        QVERIFY2(req.config.contextWindow >= 4096, "window must not shrink");
        QVERIFY2(req.config.contextWindow > 4096, "window must grow when output room is starved");
        QVERIFY(fill >= 0 && fill <= 100);
        QVERIFY(req.config.contextWindow >= floor);
    }

    void test_shape_userOverrideNoGrow_shedsOldest() {
        LlmRequest req;
        req.config.contextWindow = 4096;
        for (int i = 0; i < 4; ++i) {
            req.messages.append(assistantPlain(QString(3000, QLatin1Char('a'))));
        }
        req.messages.append(userMsg(QStringLiteral("newest question")));
        const int before = req.messages.size();
        const int fill = Chat::RequestBuilder::shapeRequestToWindow(req, 200, 200, false);
        QCOMPARE(req.config.contextWindow, 4096);
        QVERIFY2(req.messages.size() < before,
                 "oldest history must be shed when growth is disallowed");
        QVERIFY2(req.messages.last().role == QStringLiteral("user"),
                 "the newest user turn is never shed");
        QVERIFY(fill >= 0 && fill <= 100);
    }

    void test_shape_fitsUnchanged() {
        LlmRequest req;
        req.config.contextWindow = 16384;
        req.messages.append(userMsg(QStringLiteral("short question")));
        const int before = req.messages.size();
        const int fill = Chat::RequestBuilder::shapeRequestToWindow(req, 300, 500, true);
        QCOMPARE(req.config.contextWindow, 16384);
        QCOMPARE(req.messages.size(), before);
        QVERIFY(fill > 0 && fill <= 100);
    }
};

QTEST_APPLESS_MAIN(TestHistoryToolDigest)
#include "test-history-tool-digest.moc"
