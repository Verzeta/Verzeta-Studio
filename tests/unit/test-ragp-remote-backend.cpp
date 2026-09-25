// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "services/ragp/ragp-types.h"
#include "services/ragp/remote-ragp-backend.h"

#include <QTest>

using Ragp::Classification;
using Ragp::Intent;
using Ragp::RemoteBackend;
using Ragp::Request;
using Ragp::Target;

class TestRagpRemoteBackend : public QObject {
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

    static QByteArray wrapOllamaResponse(const QString& innerJson) {
        QString escaped = QString(innerJson).replace(QLatin1Char('"'), QStringLiteral("\\\""));
        QString outer = QStringLiteral("{\"message\":{\"role\":\"assistant\",\"content\":\"%1\"}}")
                            .arg(escaped);
        return outer.toUtf8();
    }

    static QByteArray wrapOllamaThinkingOnlyResponse(const QString& innerJson) {
        QString escaped = QString(innerJson).replace(QLatin1Char('"'), QStringLiteral("\\\""));
        QString outer = QStringLiteral("{\"message\":{\"role\":\"assistant\","
                                       "\"content\":\"\",\"thinking\":\"%1\"}}")
                            .arg(escaped);
        return outer.toUtf8();
    }

    static QByteArray wrapOllamaBothFieldsResponse(const QString& contentJson,
                                                   const QString& thinkingJson) {
        QString cont = QString(contentJson).replace(QLatin1Char('"'), QStringLiteral("\\\""));
        QString think = QString(thinkingJson).replace(QLatin1Char('"'), QStringLiteral("\\\""));
        QString outer = QStringLiteral("{\"message\":{\"role\":\"assistant\","
                                       "\"content\":\"%1\",\"thinking\":\"%2\"}}")
                            .arg(cont, think);
        return outer.toUtf8();
    }

  private slots:


    void test_prompt_includesRoster() {
        const QString p = RemoteBackend::buildPrompt(
            makeReq(QStringLiteral("@Bob hi"),
                    QStringLiteral("Alice"),
                    {QStringLiteral("Alice"), QStringLiteral("Bob"), QStringLiteral("Carol")}));
        QVERIFY(p.contains(QStringLiteral("Alice, Bob, Carol")));
    }

    void test_prompt_includesAuthorAndMessage() {
        const QString p =
            RemoteBackend::buildPrompt(makeReq(QStringLiteral("Hey @Bob"),
                                               QStringLiteral("Alice"),
                                               {QStringLiteral("Alice"), QStringLiteral("Bob")}));
        QVERIFY(p.contains(QStringLiteral("Author: Alice")));
        QVERIFY(p.contains(QStringLiteral("Hey @Bob")));
        QVERIFY2(p.indexOf(QStringLiteral("Hey @Bob")) >
                     p.indexOf(QStringLiteral("pending_action")),
                 "the input block must come AFTER the instructions/"
                 "examples, immediately before the answer point");
    }

    void test_prompt_containsAllIntentCategories() {
        const QString p = RemoteBackend::buildPrompt(
            makeReq(QStringLiteral("x"), QStringLiteral("A"), {QStringLiteral("A")}));
        const QStringList required = {
            QStringLiteral("DELEGATE_RESPONSE"),
            QStringLiteral("DELEGATE_TASK"),
            QStringLiteral("BROADCAST_REQUEST"),
            QStringLiteral("CLARIFICATION_REQUEST"),
            QStringLiteral("CORRECTION_REQUEST"),
            QStringLiteral("STATUS_CHECK"),
            QStringLiteral("HANDOFF_COMPLETE"),
            QStringLiteral("PLURAL_ADDRESS"),
            QStringLiteral("REFERENCE"),
            QStringLiteral("ACKNOWLEDGMENT"),
            QStringLiteral("SUMMARY_LIST"),
            QStringLiteral("QUESTION_ABOUT"),
            QStringLiteral("QUOTING"),
            QStringLiteral("GREETING_FAREWELL"),
            QStringLiteral("UNKNOWN"),
        };
        for (const QString& cat : required) {
            QVERIFY2(p.contains(cat), qPrintable(cat));
        }
    }

    void test_prompt_containsRulesAndSchema() {
        const QString p = RemoteBackend::buildPrompt(
            makeReq(QStringLiteral("x"), QStringLiteral("A"), {QStringLiteral("A")}));
        QVERIFY(p.contains(QStringLiteral("OUTPUT ONLY THE JSON")));
        QVERIFY(p.contains(QStringLiteral("self-mentions")));
        QVERIFY(p.contains(QStringLiteral("\"targets\"")));
        QVERIFY(p.contains(QStringLiteral("\"confidence\"")));
        QVERIFY(p.contains(QStringLiteral("pending_action")));
        QVERIFY(p.contains(QStringLiteral("RE-SENDS")));
        QVERIFY(p.contains(QStringLiteral("who=\"other\"")));
        QVERIFY(p.contains(QStringLiteral("\"target\"")));
        QVERIFY(p.contains(QStringLiteral("WHOLE MESSAGE")));
        QVERIFY(p.contains(QStringLiteral("deliver the announced copy draft")));
    }

    void test_prompt_executedToolsFactLine() {
        Request none = makeReq(QStringLiteral("x"), QStringLiteral("A"), {QStringLiteral("A")});
        QVERIFY(RemoteBackend::buildPrompt(none).contains(
            QStringLiteral("none — no tool ran in this turn")));
        Request some = none;
        some.executedToolsSummary = QStringLiteral("tools: write_file; files: styles.css");
        QVERIFY(RemoteBackend::buildPrompt(some).contains(
            QStringLiteral("tools: write_file; files: styles.css")));
    }


    void test_parse_wellFormedSingleTarget() {
        const QByteArray response =
            wrapOllamaResponse(QStringLiteral("{\"targets\":[{\"alias\":\"Bob\","
                                              "\"intent\":\"DELEGATE_RESPONSE\","
                                              "\"context\":\"\"}],\"confidence\":0.9}"));
        const Classification c = RemoteBackend::parseResponse(response);
        QCOMPARE(c.targets.size(), 1);
        QCOMPARE(c.targets[0].alias, QStringLiteral("Bob"));
        QCOMPARE(c.targets[0].intent, Intent::DELEGATE_RESPONSE);
        QCOMPARE(c.confidence, 0.9);
        QVERIFY(!c.selfPendingAction);
        QVERIFY(c.pendingActionHint.isEmpty());
    }

    void test_parse_pendingActionSelf() {
        const Classification c = RemoteBackend::parseProviderContent(
            QStringLiteral("{\"targets\":[],\"confidence\":0.9,"
                           "\"pending_action\":{\"who\":\"self\","
                           "\"what\":\"draft pricing.md\"}}"),
            QStringLiteral("remote:test"));
        QVERIFY(c.selfPendingAction);
        QCOMPARE(c.pendingActionHint, QStringLiteral("draft pricing.md"));
    }

    void test_parse_pendingActionOtherDelegate() {
        const Classification c = RemoteBackend::parseProviderContent(
            QStringLiteral("{\"targets\":[],\"confidence\":0.95,"
                           "\"pending_action\":{\"who\":\"other\","
                           "\"what\":\"call complete_task\","
                           "\"target\":\"@Robin\"}}"),
            QStringLiteral("remote:test"));
        QVERIFY(!c.selfPendingAction);
        QCOMPARE(c.pendingDelegateAlias, QStringLiteral("Robin"));
        QCOMPARE(c.pendingActionHint, QStringLiteral("call complete_task"));
        const Classification empty = RemoteBackend::parseProviderContent(
            QStringLiteral("{\"targets\":[],\"confidence\":0.9,"
                           "\"pending_action\":{\"who\":\"other\","
                           "\"what\":\"x\",\"target\":\"\"}}"),
            QStringLiteral("remote:test"));
        QVERIFY(empty.pendingDelegateAlias.isEmpty());
        QVERIFY(empty.pendingActionHint.isEmpty());
    }

    void test_parse_pendingActionNoneAndMalformed() {
        const Classification none = RemoteBackend::parseProviderContent(
            QStringLiteral("{\"targets\":[],\"confidence\":0.9,"
                           "\"pending_action\":{\"who\":\"none\",\"what\":\"x\"}}"),
            QStringLiteral("remote:test"));
        QVERIFY(!none.selfPendingAction);
        QVERIFY(none.pendingActionHint.isEmpty());
        QVERIFY(none.pendingDelegateAlias.isEmpty());
        const Classification bad =
            RemoteBackend::parseProviderContent(QStringLiteral("{\"targets\":[],\"confidence\":0.8,"
                                                               "\"pending_action\":\"self\"}"),
                                                QStringLiteral("remote:test"));
        QVERIFY(!bad.selfPendingAction);
        QCOMPARE(bad.confidence, 0.8);
    }

    void test_parse_multipleTargetsMixedIntents() {
        const QByteArray response =
            wrapOllamaResponse(QStringLiteral("{\"targets\":["
                                              "{\"alias\":\"Alice\",\"intent\":\"ACKNOWLEDGMENT\","
                                              "\"context\":\"\"},"
                                              "{\"alias\":\"Bob\",\"intent\":\"HANDOFF_COMPLETE\","
                                              "\"context\":\"\"}],"
                                              "\"confidence\":0.85}"));
        const Classification c = RemoteBackend::parseResponse(response);
        QCOMPARE(c.targets.size(), 2);
        QCOMPARE(c.targets[0].intent, Intent::ACKNOWLEDGMENT);
        QCOMPARE(c.targets[1].intent, Intent::HANDOFF_COMPLETE);
    }

    void test_parse_delegateTaskWithContext() {
        const QByteArray response =
            wrapOllamaResponse(QStringLiteral("{\"targets\":[{\"alias\":\"Writer\","
                                              "\"intent\":\"DELEGATE_TASK\","
                                              "\"context\":\"draft intro paragraph\"}],"
                                              "\"confidence\":0.95}"));
        const Classification c = RemoteBackend::parseResponse(response);
        QCOMPARE(c.targets.size(), 1);
        QCOMPARE(c.targets[0].intent, Intent::DELEGATE_TASK);
        QCOMPARE(c.targets[0].context, QStringLiteral("draft intro paragraph"));
    }

    void test_parse_unknownIntentFallback() {
        const QByteArray response =
            wrapOllamaResponse(QStringLiteral("{\"targets\":[{\"alias\":\"Bob\","
                                              "\"intent\":\"GIBBERISH_CATEGORY\"}],"
                                              "\"confidence\":0.5}"));
        const Classification c = RemoteBackend::parseResponse(response);
        QCOMPARE(c.targets.size(), 1);
        QCOMPARE(c.targets[0].intent, Intent::UNKNOWN);
    }

    void test_parse_malformedOuterJson() {
        const Classification c = RemoteBackend::parseResponse(QByteArray("this is not json"));
        QVERIFY(c.targets.isEmpty());
        QCOMPARE(c.confidence, 0.0);
    }

    void test_parse_malformedInnerJson() {
        const QByteArray response =
            QByteArrayLiteral("{\"message\":{\"content\":\"not a json object\"}}");
        const Classification c = RemoteBackend::parseResponse(response);
        QVERIFY(c.targets.isEmpty());
        QCOMPARE(c.confidence, 0.0);
    }

    void test_parse_emptyResponseField() {
        const QByteArray response = QByteArrayLiteral("{\"message\":{\"content\":\"\"}}");
        const Classification c = RemoteBackend::parseResponse(response);
        QVERIFY(c.targets.isEmpty());
    }

    void test_parse_missingTargetsArray() {
        const QByteArray response = wrapOllamaResponse(QStringLiteral("{\"confidence\":0.5}"));
        const Classification c = RemoteBackend::parseResponse(response);
        QVERIFY(c.targets.isEmpty());
        QCOMPARE(c.confidence, 0.5);
    }

    void test_parse_confidenceClampedToRange() {
        const QByteArray responseHigh =
            wrapOllamaResponse(QStringLiteral("{\"targets\":[{\"alias\":\"Bob\","
                                              "\"intent\":\"REFERENCE\"}],\"confidence\":1.5}"));
        const Classification cHigh = RemoteBackend::parseResponse(responseHigh);
        QCOMPARE(cHigh.confidence, 1.0);

        const QByteArray responseLow =
            wrapOllamaResponse(QStringLiteral("{\"targets\":[{\"alias\":\"Bob\","
                                              "\"intent\":\"REFERENCE\"}],\"confidence\":-0.3}"));
        const Classification cLow = RemoteBackend::parseResponse(responseLow);
        QCOMPARE(cLow.confidence, 0.0);
    }

    void test_parse_skipsTargetWithEmptyAlias() {
        const QByteArray response =
            wrapOllamaResponse(QStringLiteral("{\"targets\":["
                                              "{\"alias\":\"\",\"intent\":\"DELEGATE_RESPONSE\"},"
                                              "{\"alias\":\"Bob\",\"intent\":\"REFERENCE\"}],"
                                              "\"confidence\":0.8}"));
        const Classification c = RemoteBackend::parseResponse(response);
        QCOMPARE(c.targets.size(), 1);
        QCOMPARE(c.targets[0].alias, QStringLiteral("Bob"));
    }

    void test_parse_allIntentCategoriesRoundtrip() {
        const QList<Intent> all = {
            Intent::DELEGATE_RESPONSE,
            Intent::DELEGATE_TASK,
            Intent::BROADCAST_REQUEST,
            Intent::CLARIFICATION_REQUEST,
            Intent::CORRECTION_REQUEST,
            Intent::STATUS_CHECK,
            Intent::HANDOFF_COMPLETE,
            Intent::PLURAL_ADDRESS,
            Intent::REFERENCE,
            Intent::ACKNOWLEDGMENT,
            Intent::SUMMARY_LIST,
            Intent::QUESTION_ABOUT,
            Intent::QUOTING,
            Intent::GREETING_FAREWELL,
        };
        for (Intent intent : all) {
            const QString intentStr = Ragp::intentToString(intent);
            const QString innerJson =
                QStringLiteral("{\"targets\":[{\"alias\":\"X\",\"intent\":\"%1\"}],"
                               "\"confidence\":0.9}")
                    .arg(intentStr);
            const Classification c = RemoteBackend::parseResponse(wrapOllamaResponse(innerJson));
            QCOMPARE(c.targets.size(), 1);
            QCOMPARE(c.targets[0].intent, intent);
        }
    }

    void test_parse_sourceStampedCorrectly() {
        const QByteArray response =
            wrapOllamaResponse(QStringLiteral("{\"targets\":[],\"confidence\":0.0}"));
        const Classification c = RemoteBackend::parseResponse(response);
        QCOMPARE(c.source, QStringLiteral("remote:ollama"));
    }


    void test_parse_fallsBackToThinkingWhenResponseEmpty() {
        const QByteArray response = wrapOllamaThinkingOnlyResponse(
            QStringLiteral("{\"targets\":[{\"alias\":\"Bob\","
                           "\"intent\":\"DELEGATE_RESPONSE\",\"context\":\"\"}],"
                           "\"confidence\":0.9}"));
        const Classification c = RemoteBackend::parseResponse(response);
        QCOMPARE(c.targets.size(), 1);
        QCOMPARE(c.targets[0].alias, QStringLiteral("Bob"));
        QCOMPARE(c.targets[0].intent, Intent::DELEGATE_RESPONSE);
        QCOMPARE(c.confidence, 0.9);
    }

    void test_parse_prefersResponseOverThinkingWhenBothPresent() {
        const Classification c = RemoteBackend::parseResponse(wrapOllamaBothFieldsResponse(
            QStringLiteral("{\"targets\":[{\"alias\":\"FromResponse\","
                           "\"intent\":\"REFERENCE\"}],\"confidence\":0.8}"),
            QStringLiteral("{\"targets\":[{\"alias\":\"FromThinking\","
                           "\"intent\":\"ACKNOWLEDGMENT\"}],\"confidence\":0.2}")));
        QCOMPARE(c.targets.size(), 1);
        QCOMPARE(c.targets[0].alias, QStringLiteral("FromResponse"));
        QCOMPARE(c.targets[0].intent, Intent::REFERENCE);
        QCOMPARE(c.confidence, 0.8);
    }

    void test_parse_bothResponseAndThinkingEmpty() {
        const QByteArray response =
            QByteArrayLiteral("{\"message\":{\"content\":\"\",\"thinking\":\"\"}}");
        const Classification c = RemoteBackend::parseResponse(response);
        QVERIFY(c.targets.isEmpty());
        QCOMPARE(c.confidence, 0.0);
    }

    void test_parse_thinkingFieldMalformed() {
        const QByteArray response = QByteArrayLiteral("{\"message\":{\"content\":\"\","
                                                      "\"thinking\":\"not a json object\"}}");
        const Classification c = RemoteBackend::parseResponse(response);
        QVERIFY(c.targets.isEmpty());
        QCOMPARE(c.confidence, 0.0);
    }

    void test_parse_responseFieldMalformedDoesNotFallBackToThinking() {
        const QByteArray response =
            QByteArrayLiteral("{\"message\":{\"content\":\"garbage\","
                              "\"thinking\":\"{\\\"targets\\\":[{\\\"alias\\\":"
                              "\\\"Z\\\",\\\"intent\\\":\\\"REFERENCE\\\"}],"
                              "\\\"confidence\\\":0.9}\"}}");
        const Classification c = RemoteBackend::parseResponse(response);
        QVERIFY(c.targets.isEmpty());
        QCOMPARE(c.confidence, 0.0);
    }


    void test_providerParse_plainJson() {
        const Classification c =
            RemoteBackend::parseProviderContent(QStringLiteral("{\"targets\":[{\"alias\":\"Sam\","
                                                               "\"intent\":\"DELEGATE_TASK\","
                                                               "\"context\":\"draft spec\"}],"
                                                               "\"confidence\":0.9}"),
                                                QStringLiteral("remote:openai"));
        QCOMPARE(c.targets.size(), 1);
        QCOMPARE(c.targets[0].alias, QStringLiteral("Sam"));
        QCOMPARE(c.targets[0].intent, Intent::DELEGATE_TASK);
        QCOMPARE(c.targets[0].context, QStringLiteral("draft spec"));
        QCOMPARE(c.confidence, 0.9);
        QCOMPARE(c.source, QStringLiteral("remote:openai"));
    }

    void test_providerParse_markdownFencedJson() {
        const Classification c =
            RemoteBackend::parseProviderContent(QStringLiteral("```json\n{\"targets\":[],"
                                                               "\"confidence\":0.95}\n```"),
                                                QStringLiteral("remote:anthropic"));
        QVERIFY(c.targets.isEmpty());
        QCOMPARE(c.confidence, 0.95);
    }

    void test_providerParse_surroundingProse() {
        const Classification c = RemoteBackend::parseProviderContent(
            QStringLiteral("Here is the classification you asked for: "
                           "{\"targets\":[{\"alias\":\"Kate\","
                           "\"intent\":\"PLURAL_ADDRESS\","
                           "\"context\":\"\"}],\"confidence\":0.8} "
                           "Hope that helps!"),
            QStringLiteral("remote:gemini"));
        QCOMPARE(c.targets.size(), 1);
        QCOMPARE(c.targets[0].alias, QStringLiteral("Kate"));
        QCOMPARE(c.confidence, 0.8);
    }

    void test_providerParse_noJsonAtAll() {
        const Classification c = RemoteBackend::parseProviderContent(
            QStringLiteral("I could not classify this message."),
            QStringLiteral("remote:openrouter"));
        QVERIFY(c.targets.isEmpty());
        QCOMPARE(c.confidence, 0.0);
    }

    void test_providerParse_emptyContent() {
        const Classification c =
            RemoteBackend::parseProviderContent(QString(), QStringLiteral("remote:deepseek"));
        QVERIFY(c.targets.isEmpty());
        QCOMPARE(c.confidence, 0.0);
    }

    void test_providerParse_confidenceClamped() {
        const Classification hi = RemoteBackend::parseProviderContent(
            QStringLiteral("{\"targets\":[],\"confidence\":7.5}"), QStringLiteral("remote:x"));
        QCOMPARE(hi.confidence, 1.0);
        const Classification lo = RemoteBackend::parseProviderContent(
            QStringLiteral("{\"targets\":[],\"confidence\":-2.0}"), QStringLiteral("remote:x"));
        QCOMPARE(lo.confidence, 0.0);
    }

    void test_providerParse_malformedJsonObject() {
        const Classification c = RemoteBackend::parseProviderContent(
            QStringLiteral("{\"targets\": [unterminated"), QStringLiteral("remote:x"));
        QVERIFY(c.targets.isEmpty());
        QCOMPARE(c.confidence, 0.0);
    }
};


QTEST_MAIN(TestRagpRemoteBackend)
#include "test-ragp-remote-backend.moc"
