// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: GPL-3.0-or-later


#include "services/voice-model-downloader.h"
#include "voice/voice-model-catalog.h"

#include <QTemporaryDir>
#include <QTest>

#include <QFile>
#include <QObject>
#include <QRegularExpression>
#include <QSet>
#include <QSignalSpy>

using Verzeta::Voice::voiceModelCatalog;
using Verzeta::Voice::VoiceModelDownloader;
using Verzeta::Voice::VoiceModelEntry;

class TestVoiceModelDownloader : public QObject {
    Q_OBJECT

  private slots:

    void test_catalogEntriesAreWellFormed() {
        const QList<VoiceModelEntry> entries = voiceModelCatalog();
        QVERIFY(entries.size() >= 7);

        const QRegularExpression shaShape(QStringLiteral("^[0-9a-f]{64}$"));
        QSet<QString> ids;
        int voices = 0;
        int stt = 0;
        for (const VoiceModelEntry& e : entries) {
            QVERIFY2(!ids.contains(e.id), qPrintable(e.id));
            ids.insert(e.id);
            QVERIFY(!e.displayName.isEmpty());
            QVERIFY2(!e.license.isEmpty(), qPrintable(e.id));
            QVERIFY(e.kind == QLatin1String("voice") || e.kind == QLatin1String("stt"));
            if (e.kind == QLatin1String("voice")) {
                ++voices;
                QCOMPARE(e.files.size(), 2);
                QVERIFY(e.files.at(0).fileName.endsWith(QLatin1String(".onnx")));
                QVERIFY(e.files.at(1).fileName.endsWith(QLatin1String(".onnx.json")));
            } else {
                ++stt;
                QCOMPARE(e.files.size(), 1);
                QVERIFY(e.files.at(0).fileName.endsWith(QLatin1String(".bin")));
            }
            qint64 total = 0;
            for (const auto& f : e.files) {
                QVERIFY2(f.url.startsWith(QLatin1String("https://")), qPrintable(f.url));
                QVERIFY2(f.sizeBytes > 0, qPrintable(f.fileName));
                QVERIFY2(shaShape.match(f.sha256).hasMatch(), qPrintable(f.fileName));
                total += f.sizeBytes;
            }
            QCOMPARE(e.totalBytes(), total);
        }
        QVERIFY(voices >= 5);
        QVERIFY(stt >= 2);
    }

    void test_catalogCarriesNoNonCommercialLicence() {
        const QList<VoiceModelEntry> entries = voiceModelCatalog();
        for (const VoiceModelEntry& e : entries) {
            QVERIFY2(!e.license.contains(QLatin1String("NC")), qPrintable(e.id));
        }
    }


    void test_verifierAcceptsAndRejects() {
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("blob"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("verzeta voice test payload");
        f.close();
        const QString good =
            QStringLiteral("f6f36063816352ab2a4f2f94f97d2230ee8af8abf86a5851bcd03333"
                           "c620ff4d");
        QVERIFY(VoiceModelDownloader::verifyFileSha256(path, good));
        QVERIFY(VoiceModelDownloader::verifyFileSha256(path, good.toUpper()));
        QVERIFY(!VoiceModelDownloader::verifyFileSha256(path, QString(64, QLatin1Char('0'))));
        QVERIFY(
            !VoiceModelDownloader::verifyFileSha256(dir.filePath(QStringLiteral("missing")), good));
    }


    void test_installedRequiresEveryFileAtExactSize() {
        QTemporaryDir dir;
        VoiceModelDownloader dl;
        dl.setTargetDir(dir.path());

        const VoiceModelEntry voice = voiceModelCatalog().first();
        QCOMPARE(voice.kind, QLatin1String("voice"));
        QVERIFY(!dl.isInstalled(voice.id));

        auto writeSized = [&dir](const QString& name, qint64 size) {
            QFile f(dir.filePath(name));
            QVERIFY(f.open(QIODevice::WriteOnly));
            QVERIFY(f.resize(size));
            f.close();
        };
        writeSized(voice.files.at(0).fileName, voice.files.at(0).sizeBytes);
        QVERIFY(!dl.isInstalled(voice.id));

        writeSized(voice.files.at(1).fileName, 1);
        QVERIFY(!dl.isInstalled(voice.id));

        writeSized(voice.files.at(1).fileName, voice.files.at(1).sizeBytes);
        QVERIFY(dl.isInstalled(voice.id));

        bool found = false;
        const QVariantList rows = dl.catalog();
        for (const QVariant& r : rows) {
            const QVariantMap row = r.toMap();
            if (row.value(QStringLiteral("id")).toString() != voice.id)
                continue;
            found = true;
            QVERIFY(row.value(QStringLiteral("installed")).toBool());
        }
        QVERIFY(found);
    }

    void test_noTargetDirMeansNothingIsInstalled() {
        VoiceModelDownloader dl;
        QVERIFY(!dl.isInstalled(voiceModelCatalog().first().id));
    }


    void test_downloadRefusesUnknownIdAndMissingDir() {
        VoiceModelDownloader dl;
        QSignalSpy done(&dl, &VoiceModelDownloader::finished);

        dl.download(voiceModelCatalog().first().id);
        QCOMPARE(done.size(), 1);
        QCOMPARE(done.last().at(1).toBool(), false);

        QTemporaryDir dir;
        dl.setTargetDir(dir.path());
        dl.download(QStringLiteral("no-such-model"));
        QCOMPARE(done.size(), 2);
        QCOMPARE(done.last().at(1).toBool(), false);
        QVERIFY(dl.activeId().isEmpty());
    }

    void test_cancelWithNothingActiveIsSilent() {
        VoiceModelDownloader dl;
        QSignalSpy done(&dl, &VoiceModelDownloader::finished);
        dl.cancel();
        QVERIFY(done.isEmpty());
    }
};

QTEST_MAIN(TestVoiceModelDownloader)
#include "test-voice-model-downloader.moc"
