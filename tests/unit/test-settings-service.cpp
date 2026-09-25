// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/db-manager.h"
#include "services/settings-service.h"
#include "utils/process-sandbox.h"

#include <QTemporaryDir>
#include <QtTest>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QStandardPaths>

class TestSettingsService : public QObject {
    Q_OBJECT

  private slots:

    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void cleanupTestCase() { QStandardPaths::setTestModeEnabled(false); }

    void init() {
        QVERIFY(m_tempDir.isValid());
        m_dbPath = m_tempDir.path() +
                   QStringLiteral("/settings_%1.db").arg(QDateTime::currentMSecsSinceEpoch());

        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(m_dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_svc = std::make_unique<SettingsService>(DbManager::instance());

        const QString modelsDir = m_svc->ragpModelsDir();
        QDir(modelsDir).removeRecursively();
    }

    void cleanup() {
        m_svc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QFile::remove(m_dbPath);
    }


    void testShellAllowList_unsetReturnsCompiledDefault() {
        QCOMPARE(m_svc->shellAllowList(), ProcessSandbox::defaultAllowList());
    }

    void testShellAllowList_roundtripAddAndRemove() {
        QSignalSpy spy(m_svc.get(), &SettingsService::shellAllowListChanged);
        QStringList edited = ProcessSandbox::defaultAllowList();
        edited.removeAll(QStringLiteral("curl"));
        edited.append(QStringLiteral("mytool"));
        m_svc->setShellAllowList(edited);

        QCOMPARE(spy.count(), 1);
        const QStringList got = m_svc->shellAllowList();
        QVERIFY(!got.contains(QStringLiteral("curl")));
        QVERIFY(got.contains(QStringLiteral("mytool")));
        QVERIFY(got.contains(QStringLiteral("npm")));
    }

    void testShellAllowList_persistsAcrossInstances() {
        m_svc->setShellAllowList({QStringLiteral("ls"), QStringLiteral("git")});
        auto svc2 = std::make_unique<SettingsService>(DbManager::instance());
        QCOMPARE(svc2->shellAllowList(),
                 (QStringList{QStringLiteral("ls"), QStringLiteral("git")}));
    }

    void testShellAllowList_resetRestoresDefault() {
        m_svc->setShellAllowList({QStringLiteral("ls")});
        QVERIFY(m_svc->shellAllowList() != ProcessSandbox::defaultAllowList());
        QSignalSpy spy(m_svc.get(), &SettingsService::shellAllowListChanged);
        m_svc->resetShellAllowListToDefault();
        QCOMPARE(spy.count(), 1);
        QCOMPARE(m_svc->shellAllowList(), ProcessSandbox::defaultAllowList());
    }

    void testShellAllowList_dedupesAndTrims() {
        m_svc->setShellAllowList({QStringLiteral("ls"),
                                  QStringLiteral(" ls "),
                                  QStringLiteral("git"),
                                  QStringLiteral("")});
        QCOMPARE(m_svc->shellAllowList(),
                 (QStringList{QStringLiteral("ls"), QStringLiteral("git")}));
    }


    void testRagpLocalEnabled_defaultIsFalse() { QCOMPARE(m_svc->ragpLocalEnabled(), false); }

    void testRagpLocalEnabled_roundtrip() {
        QSignalSpy spyChanged(m_svc.get(), &SettingsService::ragpLocalEnabledChanged);
        QSignalSpy spyConfig(m_svc.get(), &SettingsService::ragpBackendConfigChanged);

        m_svc->setRagpLocalEnabled(true);
        QCOMPARE(m_svc->ragpLocalEnabled(), true);
        QCOMPARE(spyChanged.count(), 1);
        QCOMPARE(spyConfig.count(), 1);

        m_svc->setRagpLocalEnabled(true);
        QCOMPARE(spyChanged.count(), 1);
        QCOMPARE(spyConfig.count(), 1);

        m_svc->setRagpLocalEnabled(false);
        QCOMPARE(m_svc->ragpLocalEnabled(), false);
        QCOMPARE(spyChanged.count(), 2);
        QCOMPARE(spyConfig.count(), 2);
    }

    void testRagpLocalEnabled_persistsAcrossInstances() {
        m_svc->setRagpLocalEnabled(true);
        m_svc.reset();

        auto svc2 = std::make_unique<SettingsService>(DbManager::instance());
        QCOMPARE(svc2->ragpLocalEnabled(), true);
    }


    void testRagpDefaultModelFilename_roundtrip() {
        QSignalSpy spyChanged(m_svc.get(), &SettingsService::ragpDefaultModelFilenameChanged);
        QSignalSpy spyConfig(m_svc.get(), &SettingsService::ragpBackendConfigChanged);

        m_svc->setRagpDefaultModelFilename(QStringLiteral("phi-4-mini-q4.gguf"));
        QCOMPARE(m_svc->ragpDefaultModelFilename(), QStringLiteral("phi-4-mini-q4.gguf"));
        QCOMPARE(spyChanged.count(), 1);
        QCOMPARE(spyConfig.count(), 1);

        m_svc->setRagpDefaultModelFilename(QStringLiteral("phi-4-mini-q4.gguf"));
        QCOMPARE(spyChanged.count(), 1);
    }

    void testRagpDefaultModelFilename_rejectsPathSeparators() {
        m_svc->setRagpDefaultModelFilename(QStringLiteral("../evil/path.gguf"));
        QCOMPARE(m_svc->ragpDefaultModelFilename(), QString{});

        m_svc->setRagpDefaultModelFilename(QStringLiteral("subdir\\evil.gguf"));
        QCOMPARE(m_svc->ragpDefaultModelFilename(), QString{});

        m_svc->setRagpDefaultModelFilename(QStringLiteral("valid.gguf"));
        QCOMPARE(m_svc->ragpDefaultModelFilename(), QStringLiteral("valid.gguf"));
    }


    void testRagpModelsDir_underAppLocalDataLocation() {
        const QString dir = m_svc->ragpModelsDir();
        QVERIFY(!dir.isEmpty());
        QVERIFY2(dir.endsWith(QStringLiteral("/ragp-models")),
                 qPrintable(QStringLiteral("got: ") + dir));
    }

    void testRagpAvailableModels_emptyDirReturnsEmptyList() {
        const QStringList models = m_svc->ragpAvailableModels();
        QVERIFY(models.isEmpty());
        QVERIFY(QDir(m_svc->ragpModelsDir()).exists());
    }

    void testRagpAvailableModels_findsGgufFiles() {
        QDir dir(m_svc->ragpModelsDir());
        QVERIFY(dir.mkpath(QStringLiteral(".")));

        for (const QString& name : {
                 QStringLiteral("phi-4-mini-q4.gguf"),
                 QStringLiteral("smollm3-3b-q5.gguf"),
                 QStringLiteral("NOT_A_MODEL.txt"),
                 QStringLiteral("also-not.md"),
             }) {
            QFile f(dir.absoluteFilePath(name));
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.close();
        }

        const QStringList models = m_svc->ragpAvailableModels();
        QCOMPARE(models.size(), 2);
        QVERIFY(models.contains(QStringLiteral("phi-4-mini-q4.gguf")));
        QVERIFY(models.contains(QStringLiteral("smollm3-3b-q5.gguf")));
    }

    void testRagpAvailableModels_caseInsensitiveMatch() {
        QDir dir(m_svc->ragpModelsDir());
        QVERIFY(dir.mkpath(QStringLiteral(".")));

        QFile f(dir.absoluteFilePath(QStringLiteral("UPPER.GGUF")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();

        const QStringList models = m_svc->ragpAvailableModels();
        QCOMPARE(models.size(), 1);
        QCOMPARE(models[0], QStringLiteral("UPPER.GGUF"));
    }

    void testRagpAvailableModels_emitsChangedSignalOnlyOnSetChange() {
        QSignalSpy spy(m_svc.get(), &SettingsService::ragpAvailableModelsChanged);

        m_svc->ragpAvailableModels();
        QCOMPARE(spy.count(), 0);

        m_svc->ragpAvailableModels();
        m_svc->ragpAvailableModels();
        QCOMPARE(spy.count(), 0);

        QDir dir(m_svc->ragpModelsDir());
        QFile f(dir.absoluteFilePath(QStringLiteral("new.gguf")));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();

        m_svc->ragpAvailableModels();
        QCOMPARE(spy.count(), 1);

        m_svc->ragpAvailableModels();
        QCOMPARE(spy.count(), 1);
    }


    void testRagpModelPath_resolvesToAbsolutePath() {
        const QString p = m_svc->ragpModelPath(QStringLiteral("phi-4-mini-q4.gguf"));
        QVERIFY(p.endsWith(QStringLiteral("/ragp-models/phi-4-mini-q4.gguf")));
        const QString root = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
        QVERIFY2(p.startsWith(root), qPrintable(p));
    }

    void testRagpModelPath_emptyFilenameReturnsEmptyString() {
        QCOMPARE(m_svc->ragpModelPath(QString{}), QString{});
    }

    void testRagpModelPath_stripsPathSeparatorsDefensively() {
        const QString p = m_svc->ragpModelPath(QStringLiteral("../../escape.gguf"));
        QVERIFY(p.endsWith(QStringLiteral("/ragp-models/escape.gguf")));
        QVERIFY2(!p.contains(QStringLiteral("../")), qPrintable(p));
    }


    void testRagpAccelerationBackend_nonEmpty() {
        const QString b = m_svc->ragpAccelerationBackend();
        QVERIFY(!b.isEmpty());
        const QStringList valid = {
            QStringLiteral("sidecar"),
            QStringLiteral("unavailable"),
        };
        QVERIFY2(valid.contains(b), qPrintable(b));
    }


    void testIsProviderConfigured_StartsFalse() {
        for (const auto& id :
             {"openai", "anthropic", "gemini", "openrouter", "deepseek", "llamacpp_remote"}) {
            m_svc->setApiKey(id, "");
        }
        QVERIFY(!m_svc->isProviderConfigured("openai"));
        QVERIFY(!m_svc->isProviderConfigured("anthropic"));
        QVERIFY(!m_svc->isProviderConfigured("gemini"));
        QVERIFY(!m_svc->isProviderConfigured("openrouter"));
        QVERIFY(!m_svc->isProviderConfigured("deepseek"));
        QVERIFY(!m_svc->isProviderConfigured("llamacpp_local"));
        QVERIFY(!m_svc->isProviderConfigured("llamacpp_remote"));
        QVERIFY(!m_svc->isProviderConfigured("ollama"));
    }

    void testIsProviderConfigured_CloudFlipsWithApiKey() {
        m_svc->setApiKey("openai", "");
        QVERIFY(!m_svc->isProviderConfigured("openai"));
        m_svc->setApiKey("openai", "sk-test");
        QVERIFY(m_svc->isProviderConfigured("openai"));
        m_svc->setApiKey("openai", "");
        QVERIFY(!m_svc->isProviderConfigured("openai"));
    }

    void testIsProviderConfigured_OllamaNeedsExplicitRow() {
        QVERIFY(!m_svc->hasSetting("ollama_base_url"));
        QVERIFY(!m_svc->isProviderConfigured("ollama"));
        m_svc->setOllamaBaseUrl("http://localhost:11434");
        QVERIFY(m_svc->hasSetting("ollama_base_url"));
        QVERIFY(m_svc->isProviderConfigured("ollama"));
    }

    void testIsProviderConfigured_LlamaLocalNeedsPath() {
        QVERIFY(!m_svc->isProviderConfigured("llamacpp_local"));
        m_svc->setLlamaCppModelPath("/tmp/fake.gguf");
        QVERIFY(m_svc->isProviderConfigured("llamacpp_local"));
        m_svc->setLlamaCppModelPath("");
        QVERIFY(!m_svc->isProviderConfigured("llamacpp_local"));
    }

    void testIsProviderConfigured_LlamaRemoteNeedsUrl() {
        QVERIFY(!m_svc->isProviderConfigured("llamacpp_remote"));
        m_svc->setProviderBaseUrl("llamacpp_remote", "http://192.168.0.10:8080/v1");
        QVERIFY(m_svc->isProviderConfigured("llamacpp_remote"));
        m_svc->setProviderBaseUrl("llamacpp_remote", "");
        QVERIFY(!m_svc->isProviderConfigured("llamacpp_remote"));
    }

    void testIsProviderConfigured_UnknownReturnsFalse() {
        QVERIFY(!m_svc->isProviderConfigured("not-a-real-provider"));
        QVERIFY(!m_svc->isProviderConfigured(""));
    }


  private:
    QString makeExternalGgufFixture(const QString& basename) {
        const QString path = m_tempDir.path() + QStringLiteral("/external_") +
                             QString::number(QDateTime::currentMSecsSinceEpoch()) +
                             QLatin1Char('_') + basename;
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return QString();
        f.write("GGUF");
        f.write("....padding....");
        f.close();
        return path;
    }

    QString makeBogusNonGgufFile(const QString& basename) {
        const QString path = m_tempDir.path() + QStringLiteral("/bogus_") + basename;
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return QString();
        f.write("NOT A GGUF");
        f.close();
        return path;
    }

  private slots:

    void testAddModel_happyPathCreatesSymlink() {
        const QString extPath = makeExternalGgufFixture(QStringLiteral("phi-4.gguf"));
        QVERIFY(!extPath.isEmpty());

        QSignalSpy scanSpy(m_svc.get(), &SettingsService::ragpAvailableModelsChanged);

        const QString err = m_svc->ragpAddModelFromPath(extPath);
        QCOMPARE(err, QString{});

        const QStringList models = m_svc->ragpAvailableModels();
        QVERIFY2(
            models.contains(QFileInfo(extPath).fileName()),
            qPrintable(QStringLiteral("models list: [%1]").arg(models.join(QStringLiteral(", ")))));

        QVERIFY(scanSpy.count() >= 1);

        const QDir modelsDir(m_svc->ragpModelsDir());
        QFileInfo linkInfo(modelsDir.absoluteFilePath(QFileInfo(extPath).fileName()));
        QVERIFY2(linkInfo.isSymLink(), "Target in models dir must be a symlink, not a copy");
        QCOMPARE(linkInfo.symLinkTarget(), extPath);
    }

    void testAddModel_rejectsEmptyPath() {
        const QString err = m_svc->ragpAddModelFromPath(QString{});
        QVERIFY2(!err.isEmpty(), "Empty path must produce a non-empty error message");
    }

    void testAddModel_rejectsRelativePath() {
        const QString err = m_svc->ragpAddModelFromPath(QStringLiteral("./relative.gguf"));
        QVERIFY2(!err.isEmpty(), "Relative paths must be rejected");
    }

    void testAddModel_rejectsNonExistentFile() {
        const QString err =
            m_svc->ragpAddModelFromPath(QStringLiteral("/definitely/does/not/exist.gguf"));
        QVERIFY2(!err.isEmpty(), "Non-existent file must be rejected");
    }

    void testAddModel_rejectsWrongExtension() {
        const QString extPath = m_tempDir.path() + QStringLiteral("/not-a-model.txt");
        QFile f(extPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("GGUF");
        f.close();

        const QString err = m_svc->ragpAddModelFromPath(extPath);
        QVERIFY2(!err.isEmpty(),
                 "Non-.gguf extension must be rejected (extension "
                 "check runs before magic sniff)");
    }

    void testAddModel_rejectsMissingMagic() {
        const QString bogus = makeBogusNonGgufFile(QStringLiteral("fake.gguf"));
        QVERIFY(!bogus.isEmpty());

        const QString err = m_svc->ragpAddModelFromPath(bogus);
        QVERIFY2(!err.isEmpty(),
                 "File with .gguf extension but without GGUF magic "
                 "bytes must be rejected");
    }

    void testAddModel_isIdempotentForSameSource() {
        const QString extPath = makeExternalGgufFixture(QStringLiteral("idem.gguf"));
        QVERIFY(!extPath.isEmpty());

        QCOMPARE(m_svc->ragpAddModelFromPath(extPath), QString{});
        QCOMPARE(m_svc->ragpAddModelFromPath(extPath), QString{});
    }

    void testAddModel_refusesDifferentTargetWithSameName() {
        const QString firstDir = m_tempDir.path() + QStringLiteral("/dir1");
        const QString secondDir = m_tempDir.path() + QStringLiteral("/dir2");
        QVERIFY(QDir().mkpath(firstDir));
        QVERIFY(QDir().mkpath(secondDir));

        auto writeFixture = [](const QString& path) -> bool {
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly))
                return false;
            f.write("GGUF");
            f.write("padding");
            f.close();
            return true;
        };

        const QString first = firstDir + QStringLiteral("/dup.gguf");
        const QString second = secondDir + QStringLiteral("/dup.gguf");
        QVERIFY(writeFixture(first));
        QVERIFY(writeFixture(second));

        QCOMPARE(m_svc->ragpAddModelFromPath(first), QString{});

        const QString err = m_svc->ragpAddModelFromPath(second);
        QVERIFY2(!err.isEmpty(),
                 "Adding a different file with a basename already "
                 "in the list must fail — silent overwrite would "
                 "be a silent-data-loss bug");
    }

    void testAddModel_sourceAlreadyInsideModelsDir() {
        const QDir modelsDir(m_svc->ragpModelsDir());
        QVERIFY(modelsDir.mkpath(QStringLiteral(".")));
        const QString inside = modelsDir.absoluteFilePath(QStringLiteral("already-there.gguf"));
        QFile f(inside);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("GGUF");
        f.close();

        const QString err = m_svc->ragpAddModelFromPath(inside);
        QCOMPARE(err, QString{});

        const QString resolved = modelsDir.absoluteFilePath(QStringLiteral("already-there.gguf"));
        QVERIFY2(!QFileInfo(resolved).isSymLink(),
                 "Source already inside models dir must not be "
                 "symlinked back to itself");
        QVERIFY(m_svc->ragpAvailableModels().contains(QStringLiteral("already-there.gguf")));
    }

    void testRemoveModel_removesSymlinkOnly() {
        const QString ext = makeExternalGgufFixture(QStringLiteral("removable.gguf"));
        QCOMPARE(m_svc->ragpAddModelFromPath(ext), QString{});
        QVERIFY(m_svc->ragpAvailableModels().contains(QFileInfo(ext).fileName()));

        QSignalSpy scanSpy(m_svc.get(), &SettingsService::ragpAvailableModelsChanged);
        const QString err = m_svc->ragpRemoveModel(QFileInfo(ext).fileName());
        QCOMPARE(err, QString{});

        QVERIFY(!m_svc->ragpAvailableModels().contains(QFileInfo(ext).fileName()));
        QVERIFY2(QFileInfo::exists(ext),
                 "Source file must NOT be deleted when removing "
                 "from the list");
        QVERIFY(scanSpy.count() >= 1);
    }

    void testRemoveModel_refusesToDeleteRealFile() {
        const QDir modelsDir(m_svc->ragpModelsDir());
        QVERIFY(modelsDir.mkpath(QStringLiteral(".")));
        const QString realPath = modelsDir.absoluteFilePath(QStringLiteral("real.gguf"));
        QFile f(realPath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("GGUF");
        f.close();

        const QString err = m_svc->ragpRemoveModel(QStringLiteral("real.gguf"));
        QVERIFY2(!err.isEmpty(), "Real file must be refused (safety invariant)");
        QVERIFY2(QFileInfo::exists(realPath), "Real file must still exist after a refused remove");
    }

    void testRemoveModel_rejectsMissingEntry() {
        const QString err = m_svc->ragpRemoveModel(QStringLiteral("not-in-list.gguf"));
        QVERIFY2(!err.isEmpty(), "Removing a non-existent entry must report error");
    }

    void testRemoveModel_rejectsPathTraversal() {
        const QString err = m_svc->ragpRemoveModel(QStringLiteral("../evil/escape.gguf"));
        QVERIFY2(!err.isEmpty(), "Path-separator-bearing filenames must be rejected");
    }

    void testRemoveModel_clearsDefaultWhenRemovingCurrent() {
        const QString ext = makeExternalGgufFixture(QStringLiteral("current.gguf"));
        const QString basename = QFileInfo(ext).fileName();
        QCOMPARE(m_svc->ragpAddModelFromPath(ext), QString{});

        m_svc->setRagpDefaultModelFilename(basename);
        QCOMPARE(m_svc->ragpDefaultModelFilename(), basename);

        QCOMPARE(m_svc->ragpRemoveModel(basename), QString{});
        QCOMPARE(m_svc->ragpDefaultModelFilename(), QString{});
    }


    void testEmbedAddModel_happyPathCreatesSymlinkAndLists() {
        const QString ext = makeExternalGgufFixture(QStringLiteral("mxbai-embed.gguf"));
        QVERIFY(!ext.isEmpty());

        const QString err = m_svc->embeddingAddModelFromPath(ext);
        QCOMPARE(err, QString{});

        const QString basename = QFileInfo(ext).fileName();
        QVERIFY(m_svc->embeddingAvailableModels().contains(basename));

        const QDir modelsDir(m_svc->embeddingModelsDir());
        QFileInfo linkInfo(modelsDir.absoluteFilePath(basename));
        QVERIFY2(linkInfo.isSymLink(), "Target in embedding-models dir must be a symlink");
        QCOMPARE(linkInfo.symLinkTarget(), ext);

        QVERIFY(!m_svc->ragpAvailableModels().contains(basename));
    }

    void testEmbedAddModel_rejectsMissingMagic() {
        const QString bogus = makeBogusNonGgufFile(QStringLiteral("fake-embed.gguf"));
        const QString err = m_svc->embeddingAddModelFromPath(bogus);
        QVERIFY2(!err.isEmpty(), "Non-GGUF content must be rejected by the magic sniff");
    }

    void testEmbedRemoveModel_clearsSelectionAndRefusesRealFile() {
        const QString ext = makeExternalGgufFixture(QStringLiteral("selected-embed.gguf"));
        const QString basename = QFileInfo(ext).fileName();
        QCOMPARE(m_svc->embeddingAddModelFromPath(ext), QString{});

        m_svc->setEmbeddingLocalModelFilename(basename);
        QCOMPARE(m_svc->embeddingLocalModelFilename(), basename);

        QCOMPARE(m_svc->embeddingRemoveModel(basename), QString{});
        QCOMPARE(m_svc->embeddingLocalModelFilename(), QString{});
        QVERIFY(!m_svc->embeddingAvailableModels().contains(basename));

        QDir dir(m_svc->embeddingModelsDir());
        const QString realPath = dir.absoluteFilePath(QStringLiteral("real-embed.gguf"));
        {
            QFile f(realPath);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write("GGUF....");
            f.close();
        }
        const QString err = m_svc->embeddingRemoveModel(QStringLiteral("real-embed.gguf"));
        QVERIFY2(!err.isEmpty(), "Real files must be refused");
        QVERIFY(QFile::exists(realPath));
    }

  private:
    QTemporaryDir m_tempDir;
    QString m_dbPath;
    std::unique_ptr<SettingsService> m_svc;
};

QTEST_MAIN(TestSettingsService)
#include "test-settings-service.moc"
