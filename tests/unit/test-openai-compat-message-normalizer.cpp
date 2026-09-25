// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "api/openai-compat-provider.h"

#include <QtTest/QtTest>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>

namespace {

QJsonObject msg(const QString& role, const QString& content) {
    return QJsonObject{{QStringLiteral("role"), role}, {QStringLiteral("content"), content}};
}

QString serialize(const QJsonArray& a) {
    return QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact));
}

bool isStrictValid(const QJsonArray& a) {
    QString prevRole;
    for (int i = 0; i < a.size(); ++i) {
        const QJsonObject m = a[i].toObject();
        const QString role = m.value(QStringLiteral("role")).toString();
        if (role == QStringLiteral("system") && i != 0)
            return false;
        if (!prevRole.isEmpty() && role == prevRole &&
            (role == QStringLiteral("user") || role == QStringLiteral("system"))) {
            return false;
        }
        if (role == QStringLiteral("assistant") && m.contains(QStringLiteral("tool_calls"))) {
            const QJsonValue c = m.value(QStringLiteral("content"));
            if (c.isString() && c.toString().isEmpty())
                return false;
        }
        prevRole = role;
    }
    return true;
}

}  // namespace

class TestOpenAiCompatNormalizer : public QObject {
    Q_OBJECT

  private slots:

    void noop_onValidConversation() {
        const QJsonArray in{msg(QStringLiteral("system"), QStringLiteral("sys")),
                            msg(QStringLiteral("user"), QStringLiteral("hi")),
                            msg(QStringLiteral("assistant"), QStringLiteral("hello")),
                            msg(QStringLiteral("user"), QStringLiteral("bye")),
                            msg(QStringLiteral("assistant"), QStringLiteral("cya"))};
        const QJsonArray out = OpenAICompatProvider::normalizeStrictMessages(in);
        QCOMPARE(out, in);
    }

    void reroles_midConversationSystem() {
        const QJsonArray in{msg(QStringLiteral("system"), QStringLiteral("sys")),
                            msg(QStringLiteral("user"), QStringLiteral("u1")),
                            msg(QStringLiteral("assistant"), QStringLiteral("a1")),
                            msg(QStringLiteral("system"), QStringLiteral("TASK STATUS"))};
        const QJsonArray out = OpenAICompatProvider::normalizeStrictMessages(in);
        QVERIFY(isStrictValid(out));
        QCOMPARE(out.last().toObject().value(QStringLiteral("role")).toString(),
                 QStringLiteral("user"));
        QVERIFY(serialize(out).contains(QStringLiteral("TASK STATUS")));
    }

    void merges_consecutiveUsers_preservingOrder() {
        const QJsonArray in{msg(QStringLiteral("system"), QStringLiteral("sys")),
                            msg(QStringLiteral("user"), QStringLiteral("first")),
                            msg(QStringLiteral("user"), QStringLiteral("second")),
                            msg(QStringLiteral("user"), QStringLiteral("third")),
                            msg(QStringLiteral("assistant"), QStringLiteral("a"))};
        const QJsonArray out = OpenAICompatProvider::normalizeStrictMessages(in);
        QVERIFY(isStrictValid(out));
        QCOMPARE(out.size(), 3);
        const QString merged = out[1].toObject().value(QStringLiteral("content")).toString();
        QVERIFY(merged.contains(QStringLiteral("first")));
        QVERIFY(merged.contains(QStringLiteral("second")));
        QVERIFY(merged.contains(QStringLiteral("third")));
        QVERIFY(merged.indexOf(QStringLiteral("first")) < merged.indexOf(QStringLiteral("second")));
        QVERIFY(merged.indexOf(QStringLiteral("second")) < merged.indexOf(QStringLiteral("third")));
    }

    void doesNotMergeAssistants_preservesAgentIdentity() {
        const QJsonArray in{msg(QStringLiteral("system"), QStringLiteral("sys")),
                            msg(QStringLiteral("user"), QStringLiteral("u")),
                            msg(QStringLiteral("assistant"), QStringLiteral("(Alice said) hi")),
                            msg(QStringLiteral("assistant"), QStringLiteral("(Bob said) yo"))};
        const QJsonArray out = OpenAICompatProvider::normalizeStrictMessages(in);
        QCOMPARE(out.size(), 4);
        const QString s = serialize(out);
        QVERIFY(s.contains(QStringLiteral("(Alice said)")));
        QVERIFY(s.contains(QStringLiteral("(Bob said)")));
    }

    void preservesAssistantToolPairing() {
        QJsonObject asst{{QStringLiteral("role"), QStringLiteral("assistant")},
                         {QStringLiteral("content"), QStringLiteral("calling")},
                         {QStringLiteral("tool_calls"),
                          QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("abc")}}}}};
        QJsonObject tool{{QStringLiteral("role"), QStringLiteral("tool")},
                         {QStringLiteral("tool_call_id"), QStringLiteral("abc")},
                         {QStringLiteral("content"), QStringLiteral("result")}};
        const QJsonArray in{msg(QStringLiteral("system"), QStringLiteral("sys")),
                            msg(QStringLiteral("user"), QStringLiteral("u")),
                            asst,
                            tool};
        const QJsonArray out = OpenAICompatProvider::normalizeStrictMessages(in);
        QCOMPARE(out, in);
    }

    void nullsEmptyContentOnToolCallsAssistant() {
        QJsonObject asst{{QStringLiteral("role"), QStringLiteral("assistant")},
                         {QStringLiteral("content"), QStringLiteral("")},
                         {QStringLiteral("tool_calls"),
                          QJsonArray{QJsonObject{{QStringLiteral("id"), QStringLiteral("1")}}}}};
        const QJsonArray in{msg(QStringLiteral("system"), QStringLiteral("sys")),
                            msg(QStringLiteral("user"), QStringLiteral("u")),
                            asst};
        const QJsonArray out = OpenAICompatProvider::normalizeStrictMessages(in);
        QVERIFY(out.last().toObject().value(QStringLiteral("content")).isNull());
    }

    void idempotent() {
        const QJsonArray in{msg(QStringLiteral("system"), QStringLiteral("sys")),
                            msg(QStringLiteral("user"), QStringLiteral("u1")),
                            msg(QStringLiteral("assistant"), QStringLiteral("a1")),
                            msg(QStringLiteral("system"), QStringLiteral("s2")),
                            msg(QStringLiteral("user"), QStringLiteral("u2")),
                            msg(QStringLiteral("user"), QStringLiteral("u3"))};
        const QJsonArray once = OpenAICompatProvider::normalizeStrictMessages(in);
        const QJsonArray twice = OpenAICompatProvider::normalizeStrictMessages(once);
        QVERIFY(isStrictValid(once));
        QCOMPARE(twice, once);
    }

    void realFailureShape_becomesValid_withoutLosingContent() {
        QJsonArray in;
        in.append(msg(QStringLiteral("system"), QStringLiteral("lead")));
        in.append(msg(QStringLiteral("user"), QStringLiteral("u1")));
        in.append(msg(QStringLiteral("assistant"), QStringLiteral("a1")));
        in.append(msg(QStringLiteral("system"), QStringLiteral("mid-1")));
        in.append(msg(QStringLiteral("assistant"), QStringLiteral("a2")));
        in.append(msg(QStringLiteral("system"), QStringLiteral("mid-2")));
        in.append(msg(QStringLiteral("user"), QStringLiteral("n1")));
        in.append(msg(QStringLiteral("user"), QStringLiteral("n2")));
        in.append(msg(QStringLiteral("user"), QStringLiteral("n3")));
        in.append(msg(QStringLiteral("user"), QStringLiteral("n4")));

        QVERIFY(!isStrictValid(in));
        const QJsonArray out = OpenAICompatProvider::normalizeStrictMessages(in);
        QVERIFY(isStrictValid(out));

        const QString s = serialize(out);
        const QStringList frags{QStringLiteral("lead"),
                                QStringLiteral("u1"),
                                QStringLiteral("a1"),
                                QStringLiteral("mid-1"),
                                QStringLiteral("a2"),
                                QStringLiteral("mid-2"),
                                QStringLiteral("n1"),
                                QStringLiteral("n2"),
                                QStringLiteral("n3"),
                                QStringLiteral("n4")};
        for (const QString& f : frags) {
            QVERIFY2(s.contains(f), qPrintable(QStringLiteral("content lost: %1").arg(f)));
        }
    }
};

QTEST_MAIN(TestOpenAiCompatNormalizer)
#include "test-openai-compat-message-normalizer.moc"
