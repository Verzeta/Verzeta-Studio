// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "services/audio-service.h"
#include "services/file-service.h"
#include "services/image-service.h"
#include "services/model-router.h"

#include <QTemporaryDir>
#include <QTest>

#include <QFile>
#include <QFileInfo>
#include <QObject>
#include <QSignalSpy>
#include <QStandardPaths>

class TestMultimediaPipeline : public QObject {
    Q_OBJECT

  private:
    FileService* m_fileService = nullptr;
    ModelRouter* m_router = nullptr;
    ImageService* m_imageService = nullptr;
    AudioService* m_audioService = nullptr;

  private slots:
    void init() {
        QStandardPaths::setTestModeEnabled(true);
        m_fileService = new FileService(this);
        m_router = new ModelRouter(this);
        m_imageService = new ImageService(*m_router, *m_fileService, this);
        m_audioService = new AudioService(*m_fileService, this);
    }

    void cleanup() {
        delete m_imageService;
        m_imageService = nullptr;
        delete m_audioService;
        m_audioService = nullptr;
        delete m_router;
        m_router = nullptr;
        delete m_fileService;
        m_fileService = nullptr;
    }

    void test_servicesStartAndStop() {
        QVERIFY(m_imageService != nullptr);
        QVERIFY(m_audioService != nullptr);
    }

    void test_imageCatalogEmptyOnStartup() {
        const QStringList imgs = m_imageService->conversationImages(QStringLiteral("any-conv"));
        QVERIFY(imgs.isEmpty());
    }

    void test_audioCatalogEmptyOnStartup() {
        const QStringList audio = m_audioService->conversationAudio(QStringLiteral("any-conv"));
        QVERIFY(audio.isEmpty());
    }
};

QTEST_MAIN(TestMultimediaPipeline)
#include "test-multimedia-pipeline.moc"
