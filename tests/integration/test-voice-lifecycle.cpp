// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: GPL-3.0-or-later


#include "services/voice-call-service.h"
#include "services/voice-sidecar-host.h"

#include <QTest>

#include <QCoreApplication>
#include <QFile>
#include <QKeyEvent>
#include <QObject>
#include <QSignalSpy>

using Verzeta::Voice::VoiceCallService;
using Verzeta::Voice::VoiceSidecarHost;

namespace {

QString fakeDaemonPath() {
    QString p = QCoreApplication::applicationDirPath() + QStringLiteral("/fake-voice-daemon");
#ifdef Q_OS_WIN
    p += QStringLiteral(".exe");
#endif
    return p;
}

class ModeScope {
  public:
    explicit ModeScope(const char* mode) { qputenv("FAKE_VOICE_MODE", mode); }
    ~ModeScope() { qunsetenv("FAKE_VOICE_MODE"); }
};

}  // namespace

class TestVoiceLifecycle : public QObject {
    Q_OBJECT

  private slots:
    void initTestCase() {
        QVERIFY2(QFile::exists(fakeDaemonPath()),
                 "fake-voice-daemon must be built beside this test");
    }

    void test_enable_happyPath_runsWithPackInfo() {
        VoiceCallService svc;
        svc.setExplicitPath(fakeDaemonPath());
        QVERIFY(svc.detected());

        svc.enable();
        QTRY_VERIFY_WITH_TIMEOUT(svc.running(), 5000);
        QCOMPARE(svc.packVersion(), QStringLiteral("0.0-test"));
        QCOMPARE(svc.voices().size(), 2);
        QVERIFY(svc.lastError().isEmpty());

        svc.disable();
        QTRY_VERIFY_WITH_TIMEOUT(!svc.running(), 5000);
        QVERIFY(svc.lastError().isEmpty());
    }

    void test_versionSkew_refusedWithBothVersions_noLatch() {
        ModeScope mode("skew");
        VoiceCallService svc;
        svc.setExplicitPath(fakeDaemonPath());

        QSignalSpy stateSpy(&svc, &VoiceCallService::serviceStateChanged);
        svc.enable();
        QTRY_VERIFY_WITH_TIMEOUT(!svc.lastError().isEmpty(), 5000);
        QVERIFY(!svc.running());
        QVERIFY(svc.lastError().contains(QStringLiteral("99")));
        QVERIFY(svc.lastError().contains(QStringLiteral("v1")) ||
                svc.lastError().contains(QStringLiteral("1")));
        QVERIFY(!svc.latched());
    }

    void test_greetDeadline_startFailed() {
        ModeScope mode("silent");
        VoiceCallService svc;
        svc.setExplicitPath(fakeDaemonPath());
        svc.sidecarHostForTest()->setGreetTimeoutMs(400);

        svc.enable();
        QTRY_VERIFY_WITH_TIMEOUT(!svc.lastError().isEmpty(), 5000);
        QVERIFY(!svc.running());
        QVERIFY(svc.lastError().contains(QStringLiteral("greet")));
    }

    void test_exitBeforeGreet_latchesAfterThree_detectClears() {
        ModeScope mode("die-instantly");
        VoiceCallService svc;
        svc.setExplicitPath(fakeDaemonPath());

        QSignalSpy stateSpy(&svc, &VoiceCallService::serviceStateChanged);
        for (int attempt = 1; attempt <= 3; ++attempt) {
            const int before = stateSpy.count();
            svc.enable();
            QTRY_VERIFY_WITH_TIMEOUT(stateSpy.count() > before, 5000);
            QVERIFY(!svc.running());
            QVERIFY(!svc.lastError().isEmpty());
        }
        QVERIFY(svc.latched());

        svc.enable();
        QVERIFY(svc.lastError().contains(QStringLiteral("Detect")));

        svc.detect();
        QVERIFY(!svc.latched());
    }

    void test_crashAfterGreet_surfacesAsUnexpectedStop() {
        ModeScope mode("die-after-greet");
        VoiceCallService svc;
        svc.setExplicitPath(fakeDaemonPath());

        svc.enable();
        QTRY_VERIFY_WITH_TIMEOUT(!svc.running() && !svc.lastError().isEmpty(), 5000);
        QVERIFY(svc.lastError().contains(QStringLiteral("unexpectedly")));
    }

    void test_eventRelay_reachesHostSignal() {
        ModeScope mode("emit-event");
        VoiceCallService svc;
        svc.setExplicitPath(fakeDaemonPath());

        QSignalSpy events(svc.sidecarHostForTest(), &VoiceSidecarHost::eventReceived);
        svc.enable();
        QTRY_VERIFY_WITH_TIMEOUT(svc.running(), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(events.count() >= 1, 5000);
        QCOMPARE(events.first().at(0).toString(), QStringLiteral("levels"));
        svc.disable();
        QTRY_VERIFY_WITH_TIMEOUT(!svc.running(), 5000);
    }

    void test_autostart_enablesFromConstructor() {
        const QString fake = fakeDaemonPath();
        VoiceCallService::Persistence persist;
        persist.readExplicitPath = [fake]() { return fake; };
        persist.readAutostart = []() { return true; };

        VoiceCallService svc(std::move(persist));
        QVERIFY(svc.detected());
        QTRY_VERIFY_WITH_TIMEOUT(svc.running(), 5000);
        svc.disable();
        QTRY_VERIFY_WITH_TIMEOUT(!svc.running(), 5000);
    }


  private:
    void startSpeakingCall(VoiceCallService& svc, bool group) {
        svc.setExplicitPath(fakeDaemonPath());
        svc.enable();
        QTRY_VERIFY_WITH_TIMEOUT(svc.running(), 5000);
        svc.startCall(QStringLiteral("conv-1"), {}, group);
        QTRY_VERIFY_WITH_TIMEOUT(svc.callLive(), 5000);
        svc.speakMessage(QStringLiteral("msg-1"),
                         QStringLiteral("Kate"),
                         QStringLiteral("A long answer that keeps going "
                                        "well past the point of interest."));
        QTRY_COMPARE_WITH_TIMEOUT(svc.speakingAlias(), QStringLiteral("Kate"), 5000);
    }

  private slots:
    void test_stopSpeaking_cancelsThroughTheDaemon() {
        ModeScope mode("call");
        VoiceCallService svc;
        startSpeakingCall(svc, true);
        QVERIFY(svc.shouldHoldTurn(QStringLiteral("conv-1"), QStringLiteral("msg-1")));

        QSignalSpy finished(&svc, &VoiceCallService::speechFinished);
        svc.stopSpeaking();

        QVERIFY(svc.speakingAlias().isEmpty());
        QVERIFY(!finished.isEmpty());
        QCOMPARE(finished.first().at(0).toString(), QStringLiteral("msg-1"));
        QCOMPARE(finished.first().at(1).toBool(), false);

        QTRY_VERIFY_WITH_TIMEOUT(finished.size() >= 2, 5000);
        QCOMPARE(finished.last().at(0).toString(), QStringLiteral("msg-1"));

        svc.endCall();
        svc.disable();
        QTRY_VERIFY_WITH_TIMEOUT(!svc.running(), 5000);
    }

    void test_stopSpeaking_withNoHold_stillSilencesTheAgent() {
        ModeScope mode("call");
        VoiceCallService svc;
        startSpeakingCall(svc, false);

        QSignalSpy finished(&svc, &VoiceCallService::speechFinished);
        svc.stopSpeaking();
        QVERIFY(svc.speakingAlias().isEmpty());
        QTRY_VERIFY_WITH_TIMEOUT(!finished.isEmpty(), 5000);
        QCOMPARE(finished.last().at(0).toString(), QStringLiteral("msg-1"));
        QCOMPARE(finished.last().at(1).toBool(), false);

        svc.endCall();
        svc.disable();
        QTRY_VERIFY_WITH_TIMEOUT(!svc.running(), 5000);
    }

    void test_bargeIn_releasesTheHoldBeforeTheUtterance() {
        ModeScope mode("call");
        VoiceCallService svc;
        startSpeakingCall(svc, true);

        QVERIFY(svc.shouldHoldTurn(QStringLiteral("conv-1"), QStringLiteral("msg-1")));

        QStringList order;
        connect(&svc, &VoiceCallService::speechFinished, this, [&order](const QString&, bool) {
            order << QStringLiteral("released");
        });
        connect(&svc,
                &VoiceCallService::utteranceReady,
                this,
                [&order](const QString&, const QString&) { order << QStringLiteral("utterance"); });

        svc.setTalking(true);
        svc.setTalking(false);
        QTRY_VERIFY_WITH_TIMEOUT(order.contains(QStringLiteral("utterance")), 5000);

        QCOMPARE(order.first(), QStringLiteral("released"));
        QVERIFY(order.indexOf(QStringLiteral("released")) <
                order.indexOf(QStringLiteral("utterance")));
        QVERIFY(svc.speakingAlias().isEmpty());

        svc.endCall();
        svc.disable();
        QTRY_VERIFY_WITH_TIMEOUT(!svc.running(), 5000);
    }

    void test_bargeIn_carriesTheSelectedTarget() {
        ModeScope mode("call");
        VoiceCallService svc;
        startSpeakingCall(svc, true);
        svc.setTargetAlias(QStringLiteral("Mark"));

        QSignalSpy utterance(&svc, &VoiceCallService::utteranceReady);
        svc.setTalking(true);
        svc.setTalking(false);
        QTRY_VERIFY_WITH_TIMEOUT(!utterance.isEmpty(), 5000);

        QCOMPARE(utterance.first().at(0).toString(), QStringLiteral("conv-1"));
        QCOMPARE(utterance.first().at(1).toString(), QStringLiteral("@Mark stop and listen"));

        svc.endCall();
        svc.disable();
        QTRY_VERIFY_WITH_TIMEOUT(!svc.running(), 5000);
    }

    void test_oneToOneCallNeverPacesTheCascade() {
        ModeScope mode("call");
        VoiceCallService svc;
        startSpeakingCall(svc, false);

        QVERIFY(!svc.shouldHoldTurn(QStringLiteral("conv-1"), QStringLiteral("msg-1")));

        svc.endCall();
        svc.disable();
        QTRY_VERIFY_WITH_TIMEOUT(!svc.running(), 5000);
    }

    void test_groupCallStillPacesTheCascade() {
        ModeScope mode("call");
        VoiceCallService svc;
        startSpeakingCall(svc, true);

        QVERIFY(svc.shouldHoldTurn(QStringLiteral("conv-1"), QStringLiteral("msg-1")));

        svc.endCall();
        svc.disable();
        QTRY_VERIFY_WITH_TIMEOUT(!svc.running(), 5000);
    }

    void test_stopSpeaking_withNothingSpeaking_isSilent() {
        VoiceCallService svc;
        QSignalSpy finished(&svc, &VoiceCallService::speechFinished);
        svc.stopSpeaking();
        QVERIFY(finished.isEmpty());
    }

    void test_pttHotkey_worksDuringACall_andDiesWithIt() {
        ModeScope mode("call");
        VoiceCallService svc;
        svc.setPttHotkey(QStringLiteral("F9"));
        startSpeakingCall(svc, false);

        QObject probe;
        QKeyEvent press(QEvent::KeyPress, Qt::Key_F9, Qt::NoModifier);
        QCoreApplication::sendEvent(&probe, &press);
        QVERIFY(svc.talking());

        QKeyEvent repeat(QEvent::KeyPress, Qt::Key_F9, Qt::NoModifier, QString(), true);
        QCoreApplication::sendEvent(&probe, &repeat);
        QVERIFY(svc.talking());

        QKeyEvent release(QEvent::KeyRelease, Qt::Key_F9, Qt::NoModifier);
        QCoreApplication::sendEvent(&probe, &release);
        QVERIFY(!svc.talking());

        QKeyEvent other(QEvent::KeyPress, Qt::Key_F8, Qt::NoModifier);
        QCoreApplication::sendEvent(&probe, &other);
        QVERIFY(!svc.talking());

        svc.endCall();
        QKeyEvent afterEnd(QEvent::KeyPress, Qt::Key_F9, Qt::NoModifier);
        QCoreApplication::sendEvent(&probe, &afterEnd);
        QVERIFY(!svc.talking());

        svc.disable();
        QTRY_VERIFY_WITH_TIMEOUT(!svc.running(), 5000);
    }

    void test_destructionWhileRunning_doesNotHang() {
        {
            VoiceCallService svc;
            svc.setExplicitPath(fakeDaemonPath());
            svc.enable();
            QTRY_VERIFY_WITH_TIMEOUT(svc.running(), 5000);
        }
        QVERIFY(true);
    }
};

QTEST_MAIN(TestVoiceLifecycle)
#include "test-voice-lifecycle.moc"
