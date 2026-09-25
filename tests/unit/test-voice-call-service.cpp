// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: GPL-3.0-or-later


#include "services/voice-call-service.h"

#include <QTemporaryDir>
#include <QTest>

#include <algorithm>
#include <QFile>
#include <QKeyEvent>
#include <QObject>
#include <QSet>
#include <QSignalSpy>

using Verzeta::Voice::VoiceCallService;

namespace {

QString makeFakeExe(const QString& dir, const QString& name) {
    const QString path = dir + QLatin1Char('/') + name;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return {};
    f.write("#!/bin/sh\nexit 0\n");
    f.close();
    f.setPermissions(QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
    return path;
}

QString makePlainFile(const QString& dir, const QString& name) {
    const QString path = dir + QLatin1Char('/') + name;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly))
        return {};
    f.write("not a binary\n");
    f.close();
    f.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    return path;
}

}  // namespace

class TestVoiceCallService : public QObject {
    Q_OBJECT

  private:
    const QString m_bin = VoiceCallService::binaryFileName();

  private slots:

    void test_voiceForMember_defaultsToNothing() {
        VoiceCallService svc;
        QVERIFY(svc.voiceForMember(QStringLiteral("conv-1"), QStringLiteral("Kate")).isEmpty());
        QVERIFY(svc.voiceForMember(QStringLiteral("conv-1"), QString()).isEmpty());
    }

    void test_unassignedMemberUsesTheDefaultVoice() {
        VoiceCallService svc;
        svc.setDefaultVoice(QStringLiteral("alto"));
        QCOMPARE(svc.voiceForMember(QStringLiteral("conv-1"), QStringLiteral("Kate")),
                 QStringLiteral("alto"));
    }

    void test_assignmentIsScopedToItsConversationAndMember() {
        VoiceCallService svc;
        svc.setDefaultVoice(QStringLiteral("alto"));
        svc.setVoiceForMember(
            QStringLiteral("conv-1"), QStringLiteral("Rob"), QStringLiteral("bass"));
        QCOMPARE(svc.voiceForMember(QStringLiteral("conv-1"), QStringLiteral("Rob")),
                 QStringLiteral("bass"));
        QCOMPARE(svc.voiceForMember(QStringLiteral("conv-1"), QStringLiteral("Kate")),
                 QStringLiteral("alto"));
        QCOMPARE(svc.voiceForMember(QStringLiteral("conv-2"), QStringLiteral("Rob")),
                 QStringLiteral("alto"));
    }

    void test_memberAliasFallsBackToTheConversationVoice() {
        VoiceCallService svc;
        svc.setDefaultVoice(QStringLiteral("alto"));
        svc.setVoiceForMember(QStringLiteral("conv-1"), QString(), QStringLiteral("tenor"));
        QCOMPARE(svc.voiceForMember(QStringLiteral("conv-1"), QString()), QStringLiteral("tenor"));
        QCOMPARE(svc.voiceForMember(QStringLiteral("conv-1"), QStringLiteral("Rob")),
                 QStringLiteral("tenor"));
        svc.setVoiceForMember(
            QStringLiteral("conv-1"), QStringLiteral("Rob"), QStringLiteral("bass"));
        QCOMPARE(svc.voiceForMember(QStringLiteral("conv-1"), QStringLiteral("Rob")),
                 QStringLiteral("bass"));
    }

    void test_clearingAnAssignmentFallsBackToTheDefault() {
        VoiceCallService svc;
        svc.setDefaultVoice(QStringLiteral("alto"));
        svc.setVoiceForMember(
            QStringLiteral("conv-1"), QStringLiteral("Rob"), QStringLiteral("bass"));
        svc.setVoiceForMember(QStringLiteral("conv-1"), QStringLiteral("Rob"), QString());
        QCOMPARE(svc.voiceForMember(QStringLiteral("conv-1"), QStringLiteral("Rob")),
                 QStringLiteral("alto"));
        QVERIFY(
            svc.assignedVoiceForMember(QStringLiteral("conv-1"), QStringLiteral("Rob")).isEmpty());
    }

    void test_emptyConversationIsNeverAssigned() {
        VoiceCallService svc;
        svc.setDefaultVoice(QStringLiteral("alto"));
        svc.setVoiceForMember(QString(), QStringLiteral("Rob"), QStringLiteral("bass"));
        QCOMPARE(svc.voiceForMember(QString(), QStringLiteral("Rob")), QStringLiteral("alto"));
    }

    void test_assignmentsArePersistedAndReloaded() {
        QVariantMap store;
        QString defaultVoice;
        VoiceCallService::Persistence persist;
        persist.readVoiceAssignments = [&store]() { return store; };
        persist.writeVoiceAssignments = [&store](const QVariantMap& v) { store = v; };
        persist.readDefaultVoice = [&defaultVoice]() { return defaultVoice; };
        persist.writeDefaultVoice = [&defaultVoice](const QString& v) { defaultVoice = v; };

        {
            VoiceCallService svc(persist);
            svc.setDefaultVoice(QStringLiteral("alto"));
            svc.setVoiceForMember(
                QStringLiteral("conv-1"), QStringLiteral("Rob"), QStringLiteral("bass"));
            svc.setVoiceForMember(QStringLiteral("conv-2"), QString(), QStringLiteral("tenor"));
        }
        QCOMPARE(
            store.value(QStringLiteral("conv-1")).toMap().value(QStringLiteral("Rob")).toString(),
            QStringLiteral("bass"));

        VoiceCallService reloaded(persist);
        QCOMPARE(reloaded.defaultVoice(), QStringLiteral("alto"));
        QCOMPARE(reloaded.voiceForMember(QStringLiteral("conv-1"), QStringLiteral("Rob")),
                 QStringLiteral("bass"));
        QCOMPARE(reloaded.voiceForMember(QStringLiteral("conv-2"), QString()),
                 QStringLiteral("tenor"));
        QCOMPARE(reloaded.voiceForMember(QStringLiteral("conv-1"), QStringLiteral("Kate")),
                 QStringLiteral("alto"));
    }

    void test_oldFlatAssignmentShapeIsIgnored() {
        QVariantMap store;
        store.insert(QStringLiteral("Rob"), QStringLiteral("bass"));
        VoiceCallService::Persistence persist;
        persist.readVoiceAssignments = [&store]() { return store; };

        VoiceCallService svc(persist);
        QVERIFY(svc.voiceForMember(QStringLiteral("Rob"), QString()).isEmpty());
        QVERIFY(svc.assignedVoiceForMember(QStringLiteral("Rob"), QString()).isEmpty());
    }


    void test_hotkeyFromEventFormatsAndRefuses() {
        QCOMPARE(VoiceCallService::hotkeyFromEvent(Qt::Key_F9, Qt::NoModifier),
                 QStringLiteral("F9"));
        QCOMPARE(VoiceCallService::hotkeyFromEvent(Qt::Key_Space, Qt::ControlModifier),
                 QKeySequence(Qt::CTRL | Qt::Key_Space).toString(QKeySequence::PortableText));
        QVERIFY(VoiceCallService::hotkeyFromEvent(Qt::Key_Control, Qt::ControlModifier).isEmpty());
        QVERIFY(VoiceCallService::hotkeyFromEvent(Qt::Key_Shift, Qt::ShiftModifier).isEmpty());
    }

    void test_pttHotkeyIsPersistedAndReloaded() {
        QString stored;
        VoiceCallService::Persistence persist;
        persist.readPttHotkey = [&stored]() { return stored; };
        persist.writePttHotkey = [&stored](const QString& v) { stored = v; };

        {
            VoiceCallService svc(persist);
            QSignalSpy changed(&svc, &VoiceCallService::pttHotkeyChanged);
            svc.setPttHotkey(QStringLiteral("F9"));
            QCOMPARE(changed.size(), 1);
            svc.setPttHotkey(QStringLiteral("F9"));
            QCOMPARE(changed.size(), 1);
        }
        QCOMPARE(stored, QStringLiteral("F9"));

        VoiceCallService reloaded(persist);
        QCOMPARE(reloaded.pttHotkey(), QStringLiteral("F9"));
    }

    void test_hotkeyDoesNothingOutsideACall() {
        VoiceCallService svc;
        svc.setPttHotkey(QStringLiteral("F9"));
        QKeyEvent press(QEvent::KeyPress, Qt::Key_F9, Qt::NoModifier);
        QObject probe;
        QCoreApplication::sendEvent(&probe, &press);
        QVERIFY(!svc.talking());
    }

    void test_voiceChangesNotify() {
        VoiceCallService svc;
        QSignalSpy changed(&svc, &VoiceCallService::voiceAssignmentsChanged);
        svc.setDefaultVoice(QStringLiteral("alto"));
        QCOMPARE(changed.size(), 1);
        svc.setVoiceForMember(
            QStringLiteral("conv-1"), QStringLiteral("Rob"), QStringLiteral("bass"));
        QCOMPARE(changed.size(), 2);
        svc.setVoiceForMember(
            QStringLiteral("conv-1"), QStringLiteral("Rob"), QStringLiteral("bass"));
        QCOMPARE(changed.size(), 2);
    }

    void test_binaryFileName_isPlatformCorrect() {
#ifdef Q_OS_WIN
        QCOMPARE(m_bin, QStringLiteral("verzeta-voice.exe"));
#else
        QCOMPARE(m_bin, QStringLiteral("verzeta-voice"));
#endif
    }

    void test_resolve_nothingAnywhere_returnsEmpty() {
        QTemporaryDir a, b;
        QVERIFY(
            VoiceCallService::resolveBinaryPath({}, {a.path()}, {b.path()}, {}, {m_bin}).isEmpty());
    }

    void test_resolve_wellKnownBeatsPath() {
        QTemporaryDir wk, pathDir;
        const QString inWk = makeFakeExe(wk.path(), m_bin);
        const QString inPath = makeFakeExe(pathDir.path(), m_bin);
        QVERIFY(!inWk.isEmpty() && !inPath.isEmpty());
        QCOMPARE(
            VoiceCallService::resolveBinaryPath({}, {wk.path()}, {pathDir.path()}, {}, {m_bin}),
            inWk);
    }

    void test_resolve_pathBeatsAppDir() {
        QTemporaryDir pathDir, appDir;
        const QString inPath = makeFakeExe(pathDir.path(), m_bin);
        const QString inApp = makeFakeExe(appDir.path(), m_bin);
        QVERIFY(!inPath.isEmpty() && !inApp.isEmpty());
        QCOMPARE(
            VoiceCallService::resolveBinaryPath({}, {}, {pathDir.path()}, appDir.path(), {m_bin}),
            inPath);
    }

    void test_resolve_appDirIsLastResort() {
        QTemporaryDir appDir;
        const QString inApp = makeFakeExe(appDir.path(), m_bin);
        QVERIFY(!inApp.isEmpty());
        QCOMPARE(VoiceCallService::resolveBinaryPath({}, {}, {}, appDir.path(), {m_bin}), inApp);
    }

    void test_resolve_nonExecutableRejected() {
        QTemporaryDir wk;
        QVERIFY(!makePlainFile(wk.path(), m_bin).isEmpty());
        QVERIFY(VoiceCallService::resolveBinaryPath({}, {wk.path()}, {}, {}, {m_bin}).isEmpty());
    }

    void test_resolve_explicitWinsOverEverything() {
        QTemporaryDir expl, wk;
        const QString explicitExe = makeFakeExe(expl.path(), m_bin);
        QVERIFY(!makeFakeExe(wk.path(), m_bin).isEmpty());
        QCOMPARE(VoiceCallService::resolveBinaryPath(explicitExe, {wk.path()}, {}, {}, {m_bin}),
                 explicitExe);
    }

    void test_resolve_invalidExplicitDoesNotFallThrough() {
        QTemporaryDir wk;
        QVERIFY(!makeFakeExe(wk.path(), m_bin).isEmpty());
        QVERIFY(VoiceCallService::resolveBinaryPath(
                    QStringLiteral("/nonexistent/verzeta-voice"), {wk.path()}, {}, {}, {m_bin})
                    .isEmpty());
    }

    void test_instance_startsUndetected_thenExplicitPathDetects() {
        VoiceCallService svc;
        QVERIFY(!svc.detected());
        QVERIFY(svc.detectedPath().isEmpty());

        QTemporaryDir dir;
        const QString exe = makeFakeExe(dir.path(), m_bin);
        QVERIFY(!exe.isEmpty());

        QSignalSpy detectSpy(&svc, &VoiceCallService::detectionChanged);
        QSignalSpy pathSpy(&svc, &VoiceCallService::explicitPathChanged);

        svc.setExplicitPath(exe);
        QVERIFY(svc.detected());
        QCOMPARE(svc.detectedPath(), exe);
        QCOMPARE(detectSpy.count(), 1);
        QCOMPARE(pathSpy.count(), 1);

        svc.setExplicitPath(exe);
        QCOMPARE(detectSpy.count(), 1);
        QCOMPARE(pathSpy.count(), 1);

        svc.setExplicitPath(QString());
        QVERIFY(!svc.detected());
        QCOMPARE(detectSpy.count(), 2);
        QCOMPARE(pathSpy.count(), 2);
    }

    void test_instance_persistenceCallbacksAreUsed() {
        QTemporaryDir dir;
        const QString exe = makeFakeExe(dir.path(), m_bin);
        QVERIFY(!exe.isEmpty());

        QString stored = exe;
        QStringList writes;
        VoiceCallService::Persistence persist;
        persist.readExplicitPath = [&stored]() { return stored; };
        persist.writeExplicitPath = [&writes](const QString& v) { writes << v; };
        VoiceCallService svc(std::move(persist));

        QVERIFY(svc.detected());
        QCOMPARE(svc.explicitPath(), exe);
        QVERIFY(writes.isEmpty());

        svc.setExplicitPath(QString());
        QCOMPARE(writes.size(), 1);
        QVERIFY(writes.first().isEmpty());
    }

    void test_wellKnownDir_isUnderAppData() {
        VoiceCallService svc;
        QVERIFY(svc.wellKnownDir().endsWith(QStringLiteral("/voice")));
    }

    void test_resolve_findsAppImageByPattern() {
#ifndef Q_OS_WIN
        QTemporaryDir dir;
        const QString appImage =
            makeFakeExe(dir.path(), QStringLiteral("Verzeta-Voice-1.2.0-x86_64.AppImage"));
        QVERIFY(!appImage.isEmpty());
        QCOMPARE(VoiceCallService::resolveBinaryPath(
                     {}, {dir.path()}, {}, {}, VoiceCallService::binaryNamePatterns()),
                 appImage);
#endif
    }

    void test_resolve_prefersPlainExecutableOverAppImage() {
#ifndef Q_OS_WIN
        QTemporaryDir dir;
        QVERIFY(
            !makeFakeExe(dir.path(), QStringLiteral("Verzeta-Voice-9-x86_64.AppImage")).isEmpty());
        const QString plain = makeFakeExe(dir.path(), m_bin);
        QCOMPARE(VoiceCallService::resolveBinaryPath(
                     {}, {dir.path()}, {}, {}, VoiceCallService::binaryNamePatterns()),
                 plain);
#endif
    }

    void test_explicitPathMayBeADirectory() {
        QTemporaryDir dir;
        const QString exe = makeFakeExe(dir.path(), m_bin);
        QVERIFY(!exe.isEmpty());
        QCOMPARE(VoiceCallService::resolveBinaryPath(
                     dir.path(), {}, {}, {}, VoiceCallService::binaryNamePatterns()),
                 exe);
    }

    void test_commonInstallDirs_includePerUserAndSystemLocations() {
        const QStringList dirs = VoiceCallService::commonInstallDirs();
        QVERIFY(!dirs.isEmpty());
        QVERIFY(std::any_of(dirs.cbegin(), dirs.cend(), [](const QString& d) {
            return d.endsWith(QStringLiteral("/voice"));
        }));
        QCOMPARE(dirs.size(), QSet<QString>(dirs.cbegin(), dirs.cend()).size());
        QVERIFY(!dirs.contains(QString()));
    }

    void test_binaryNamePatterns_areNotEmpty() {
        const QStringList p = VoiceCallService::binaryNamePatterns();
        QVERIFY(p.size() >= 2);
        QVERIFY(p.first() == VoiceCallService::binaryFileName());
    }
};

QTEST_MAIN(TestVoiceCallService)
#include "test-voice-call-service.moc"
