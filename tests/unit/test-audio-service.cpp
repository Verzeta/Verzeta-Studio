// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "services/audio-service.h"
#include "services/file-service.h"

#include <QTest>

#include <QObject>
#include <QSignalSpy>
#include <QStandardPaths>


class TestAudioService : public QObject {
    Q_OBJECT

  private:
    FileService* m_fileService = nullptr;
    AudioService* m_audioService = nullptr;

  private slots:
    void init() {
        QStandardPaths::setTestModeEnabled(true);
        m_fileService = new FileService(this);
        m_audioService = new AudioService(*m_fileService, this);
    }

    void cleanup() {
        delete m_audioService;
        m_audioService = nullptr;
        delete m_fileService;
        m_fileService = nullptr;
    }


    void test_generateAudioEmitsStarted() {
        QSignalSpy spy(m_audioService, &AudioService::generationStarted);
        AudioGenConfig cfg;
        cfg.apiKey = QStringLiteral("test-key");
        m_audioService->generateAudio(QStringLiteral("conv-1"), QStringLiteral("Hello world"), cfg);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("conv-1"));
    }

    void test_generateAudioErrorWhenNoApiKey() {
        QSignalSpy spy(m_audioService, &AudioService::error);
        AudioGenConfig cfg;
        cfg.backend = QStringLiteral("openai");
        cfg.apiKey = QString();
        m_audioService->generateAudio(QStringLiteral("conv-1"), QStringLiteral("Hello"), cfg);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("conv-1"));
        QVERIFY(!spy.at(0).at(1).toString().isEmpty());
    }

    void test_generateAudioLocalTTSErrorWhenNoPath() {
        QSignalSpy spy(m_audioService, &AudioService::error);
        AudioGenConfig cfg;
        cfg.backend = QStringLiteral("local_tts");
        cfg.ttsPath = QString();
        m_audioService->generateAudio(QStringLiteral("conv-2"), QStringLiteral("Test text"), cfg);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("conv-2"));
    }

    void test_conversationAudioEmptyForUnknownConv() {
        const QStringList result =
            m_audioService->conversationAudio(QStringLiteral("no-such-conv"));
        QVERIFY(result.isEmpty());
    }

    void test_audioGenConfigDefaults() {
        AudioGenConfig cfg;
        QCOMPARE(cfg.backend, QStringLiteral("openai"));
        QCOMPARE(cfg.voice, QStringLiteral("alloy"));
        QCOMPARE(cfg.model, QStringLiteral("tts-1"));
        QCOMPARE(cfg.outputFormat, QStringLiteral("mp3"));
        QVERIFY(cfg.ttsPath.isEmpty());
    }

    void test_startedEmittedBeforeError() {
        QSignalSpy startedSpy(m_audioService, &AudioService::generationStarted);
        QSignalSpy errorSpy(m_audioService, &AudioService::error);
        AudioGenConfig cfg;
        m_audioService->generateAudio(QStringLiteral("conv-3"), QStringLiteral("test"), cfg);
        QCOMPARE(startedSpy.count(), 1);
        QCOMPARE(errorSpy.count(), 1);
    }

    void test_multipleConversationsEmitStartedForEach() {
        QSignalSpy spy(m_audioService, &AudioService::generationStarted);
        AudioGenConfig cfg;
        cfg.apiKey = QStringLiteral("key");
        m_audioService->generateAudio(QStringLiteral("c1"), QStringLiteral("Hello"), cfg);
        m_audioService->generateAudio(QStringLiteral("c2"), QStringLiteral("World"), cfg);
        QCOMPARE(spy.count(), 2);
    }
};

QTEST_MAIN(TestAudioService)
#include "test-audio-service.moc"
