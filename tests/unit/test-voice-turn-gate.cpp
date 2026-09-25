// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: GPL-3.0-or-later


#include "services/voice-call-service.h"

#include <QTest>

#include <QObject>
#include <QSignalSpy>

using Verzeta::Voice::VoiceCallService;

class TestVoiceTurnGate : public QObject {
    Q_OBJECT

  private slots:
    void test_noCall_neverHolds() {
        VoiceCallService svc;
        QVERIFY(!svc.callActive());
        QVERIFY(!svc.shouldHoldTurn(QStringLiteral("conv-1"), QStringLiteral("msg-1")));
        QVERIFY(!svc.shouldHoldTurn(QString(), QString()));
    }

    void test_emptyMessageIdNeverHolds() {
        VoiceCallService svc;
        QVERIFY(!svc.shouldHoldTurn(QStringLiteral("conv-1"), QString()));
    }

    void test_otherConversationNeverHolds() {
        VoiceCallService svc;
        QVERIFY(!svc.shouldHoldTurn(QStringLiteral("some-other-conv"), QStringLiteral("msg-1")));
    }

    void test_nothingSpeakableStillReportsCompletion() {
        VoiceCallService svc;
        QSignalSpy done(&svc, &VoiceCallService::speechFinished);
        svc.speakMessage(QStringLiteral("msg-1"), QStringLiteral("Kate"), QString());
        QVERIFY(!svc.shouldHoldTurn(QStringLiteral("conv-1"), QStringLiteral("msg-1")));
    }

    void test_repeatedQueriesWithoutCallStayFalse() {
        VoiceCallService svc;
        for (int i = 0; i < 3; ++i) {
            QVERIFY(!svc.shouldHoldTurn(QStringLiteral("conv-1"), QStringLiteral("msg-1")));
        }
    }
};

QTEST_MAIN(TestVoiceTurnGate)
#include "test-voice-turn-gate.moc"
