// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/db-manager.h"
#include "services/image-provider-registry.h"
#include "services/settings-service.h"

#include <QTemporaryDir>
#include <QtTest>

#include <QDateTime>
#include <QDir>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QStandardPaths>

class TestImageProviderRegistry : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    QString m_dbPath;
    std::unique_ptr<SettingsService> m_settings;
    std::unique_ptr<ImageProviderRegistry> m_registry;

    QVariantMap baseCfg(const QString& name, const QString& shape, const QString& url) const {
        QVariantMap m;
        m.insert(QStringLiteral("displayName"), name);
        m.insert(QStringLiteral("endpointShape"), shape);
        m.insert(QStringLiteral("baseUrl"), url);
        return m;
    }

  private slots:

    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void cleanupTestCase() { QStandardPaths::setTestModeEnabled(false); }

    void init() {
        QVERIFY(m_tempDir.isValid());
        m_dbPath = m_tempDir.path() +
                   QStringLiteral("/img_%1.db").arg(QDateTime::currentMSecsSinceEpoch());

        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(m_dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_settings = std::make_unique<SettingsService>(DbManager::instance());

        m_settings->setApiKey(QStringLiteral("openai"), QString());
        m_settings->setImageGenOpenAICompatKey(QString());
        m_settings->setImageGenA1111Key(QString());

        m_registry = std::make_unique<ImageProviderRegistry>(*m_settings);
    }

    void cleanup() {
        m_registry.reset();
        m_settings.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    }


    void test_upsert_add_returns_slug_and_lists() {
        QSignalSpy spy(m_registry.get(), &ImageProviderRegistry::providersChanged);

        QVariantMap cfg = baseCfg(QStringLiteral("OpenAI DALL-E"),
                                  QStringLiteral("openai_images"),
                                  QStringLiteral("https://api.openai.com/v1"));
        cfg.insert(QStringLiteral("model"), QStringLiteral("dall-e-3"));
        cfg.insert(QStringLiteral("apiKey"), QStringLiteral("sk-test-1"));

        const QString id = m_registry->upsert(cfg);
        QVERIFY(!id.isEmpty());
        QCOMPARE(id, QStringLiteral("openai-dall-e"));
        QCOMPARE(spy.count(), 1);

        const QVariantList rows = m_registry->list();
        QCOMPARE(rows.size(), 1);
        const QVariantMap row = rows.first().toMap();
        QCOMPARE(row.value(QStringLiteral("id")).toString(), id);
        QCOMPARE(row.value(QStringLiteral("model")).toString(), QStringLiteral("dall-e-3"));
        QCOMPARE(row.value(QStringLiteral("hasApiKey")).toBool(), true);

        QCOMPARE(m_settings->apiKey(QStringLiteral("image.openai-dall-e")),
                 QStringLiteral("sk-test-1"));
        const QString blob = m_settings->imageProvidersBlob();
        QVERIFY2(!blob.contains(QStringLiteral("sk-test-1")),
                 "raw API key leaked into the persistent blob");
    }

    void test_upsert_add_without_key_has_no_key() {
        const QString id = m_registry->upsert(
            baseCfg(QStringLiteral("Local SD"), QStringLiteral("local_cli"), QString()));
        QVERIFY(!id.isEmpty());
        const QVariantMap row = m_registry->byId(id);
        QCOMPARE(row.value(QStringLiteral("hasApiKey")).toBool(), false);
    }

    void test_upsert_rejects_empty_displayName() {
        QVariantMap cfg =
            baseCfg(QString(), QStringLiteral("openai_images"), QStringLiteral("http://x/v1"));
        QVERIFY(m_registry->upsert(cfg).isEmpty());
        QCOMPARE(m_registry->list().size(), 0);
    }

    void test_upsert_rejects_invalid_shape() {
        QVariantMap cfg = baseCfg(
            QStringLiteral("Bad"), QStringLiteral("not-a-shape"), QStringLiteral("http://x/v1"));
        QVERIFY(m_registry->upsert(cfg).isEmpty());
        QCOMPARE(m_registry->list().size(), 0);
    }


    void test_upsert_update_in_place_preserves_key_when_absent() {
        QVariantMap cfg = baseCfg(
            QStringLiteral("Server"), QStringLiteral("a1111"), QStringLiteral("http://old:7860"));
        cfg.insert(QStringLiteral("apiKey"), QStringLiteral("k-original"));
        const QString id = m_registry->upsert(cfg);
        QVERIFY(!id.isEmpty());
        QCOMPARE(m_registry->list().size(), 1);

        QVariantMap upd;
        upd.insert(QStringLiteral("id"), id);
        upd.insert(QStringLiteral("displayName"), QStringLiteral("Renamed"));
        upd.insert(QStringLiteral("endpointShape"), QStringLiteral("a1111"));
        upd.insert(QStringLiteral("baseUrl"), QStringLiteral("http://new:7860"));
        const QString id2 = m_registry->upsert(upd);
        QCOMPARE(id2, id);
        QCOMPARE(m_registry->list().size(), 1);

        const QVariantMap row = m_registry->byId(id);
        QCOMPARE(row.value(QStringLiteral("displayName")).toString(), QStringLiteral("Renamed"));
        QCOMPARE(row.value(QStringLiteral("baseUrl")).toString(),
                 QStringLiteral("http://new:7860"));
        QCOMPARE(m_settings->apiKey(QStringLiteral("image.") + id), QStringLiteral("k-original"));

        upd.insert(QStringLiteral("apiKey"), QString());
        m_registry->upsert(upd);
        QCOMPARE(m_settings->apiKey(QStringLiteral("image.") + id), QString());
        QCOMPARE(m_registry->byId(id).value(QStringLiteral("hasApiKey")).toBool(), false);
    }


    void test_byId_unknown_returns_empty() {
        QVERIFY(m_registry->byId(QStringLiteral("nope")).isEmpty());
    }


    void test_slugForBaseUrl_dup_detection() {
        const QString id = m_registry->upsert(baseCfg(QStringLiteral("Compat"),
                                                      QStringLiteral("openai_images"),
                                                      QStringLiteral("http://localhost:8000/v1")));
        QVERIFY(!id.isEmpty());

        QCOMPARE(m_registry->slugForBaseUrl(QStringLiteral("HTTP://LOCALHOST:8000/v1/")), id);
        QVERIFY(m_registry->slugForBaseUrl(QStringLiteral("http://other:9999/v1")).isEmpty());
    }


    void test_remove_clears_key_and_active() {
        QVariantMap cfg = baseCfg(QStringLiteral("To Delete"),
                                  QStringLiteral("openai_images"),
                                  QStringLiteral("http://x:1/v1"));
        cfg.insert(QStringLiteral("apiKey"), QStringLiteral("k"));
        const QString id = m_registry->upsert(cfg);
        QVERIFY(!id.isEmpty());

        m_registry->setActiveProviderId(id);
        QCOMPARE(m_registry->activeProviderId(), id);

        QSignalSpy changed(m_registry.get(), &ImageProviderRegistry::providersChanged);
        QSignalSpy activeChanged(m_registry.get(), &ImageProviderRegistry::activeProviderChanged);

        QVERIFY(m_registry->remove(id));
        QCOMPARE(changed.count(), 1);
        QCOMPARE(activeChanged.count(), 1);

        QVERIFY(m_registry->byId(id).isEmpty());
        QCOMPARE(m_settings->apiKey(QStringLiteral("image.") + id), QString());
        QVERIFY(m_registry->activeProviderId().isEmpty());
    }

    void test_remove_unknown_returns_false() {
        QVERIFY(!m_registry->remove(QStringLiteral("ghost")));
    }


    void test_active_id_roundtrips_via_settings() {
        const QString id = m_registry->upsert(baseCfg(QStringLiteral("Active One"),
                                                      QStringLiteral("openai_images"),
                                                      QStringLiteral("http://a:1/v1")));
        QVERIFY(!id.isEmpty());

        QSignalSpy spy(m_registry.get(), &ImageProviderRegistry::activeProviderChanged);
        m_registry->setActiveProviderId(id);
        QCOMPARE(spy.count(), 1);
        QCOMPARE(m_registry->activeProviderId(), id);
        QCOMPARE(m_settings->imageGenActiveBackend(), id);

        m_registry->setActiveProviderId(id);
        QCOMPARE(spy.count(), 1);
    }

    void test_activeConfig_resolves_key() {
        QVariantMap cfg = baseCfg(QStringLiteral("Cfg"),
                                  QStringLiteral("openai_images"),
                                  QStringLiteral("http://c:1/v1"));
        cfg.insert(QStringLiteral("model"), QStringLiteral("dall-e-3"));
        cfg.insert(QStringLiteral("apiKey"), QStringLiteral("sk-active"));
        const QString id = m_registry->upsert(cfg);
        m_registry->setActiveProviderId(id);

        const ActiveImageConfig active = m_registry->activeConfig();
        QVERIFY(active.valid);
        QCOMPARE(active.id, id);
        QCOMPARE(active.model, QStringLiteral("dall-e-3"));
        QCOMPARE(active.apiKey, QStringLiteral("sk-active"));
        QCOMPARE(active.endpointShape, QStringLiteral("openai_images"));
    }

    void test_activeConfig_invalid_when_none() { QVERIFY(!m_registry->activeConfig().valid); }


    void test_roundtrip_hydrate_restores_rows() {
        QVariantMap cfg = baseCfg(
            QStringLiteral("Alpha"), QStringLiteral("a1111"), QStringLiteral("http://alpha:7860"));
        cfg.insert(QStringLiteral("apiKey"), QStringLiteral("alpha-key"));
        cfg.insert(QStringLiteral("size"), QStringLiteral("512x512"));
        const QString id = m_registry->upsert(cfg);
        QVERIFY(!id.isEmpty());

        m_registry.reset();
        m_registry = std::make_unique<ImageProviderRegistry>(*m_settings);

        QCOMPARE(m_registry->list().size(), 1);
        const QVariantMap row = m_registry->byId(id);
        QVERIFY(!row.isEmpty());
        QCOMPARE(row.value(QStringLiteral("endpointShape")).toString(), QStringLiteral("a1111"));
        QCOMPARE(row.value(QStringLiteral("size")).toString(), QStringLiteral("512x512"));
        QCOMPARE(row.value(QStringLiteral("hasApiKey")).toBool(), true);
        QCOMPARE(m_settings->apiKey(QStringLiteral("image.") + id), QStringLiteral("alpha-key"));
    }


    void test_migration_seeds_rows_and_maps_active() {
        m_registry.reset();

        m_settings->setApiKey(QStringLiteral("openai"), QStringLiteral("sk-legacy"));
        m_settings->setImageGenA1111Url(QStringLiteral("http://sd:7860"));
        m_settings->setImageGenActiveBackend(QStringLiteral("remote_a1111"));

        m_registry = std::make_unique<ImageProviderRegistry>(*m_settings);

        const QVariantList rows = m_registry->list();
        QCOMPARE(rows.size(), 2);

        const QVariantMap dalle = m_registry->byId(QStringLiteral("openai-dalle"));
        QVERIFY(!dalle.isEmpty());
        QCOMPARE(dalle.value(QStringLiteral("endpointShape")).toString(),
                 QStringLiteral("openai_images"));
        QCOMPARE(dalle.value(QStringLiteral("model")).toString(), QStringLiteral("dall-e-3"));
        QCOMPARE(dalle.value(QStringLiteral("hasApiKey")).toBool(), true);
        QCOMPARE(m_settings->apiKey(QStringLiteral("image.openai-dalle")),
                 QStringLiteral("sk-legacy"));

        const QVariantMap a1111 = m_registry->byId(QStringLiteral("automatic1111"));
        QVERIFY(!a1111.isEmpty());
        QCOMPARE(a1111.value(QStringLiteral("baseUrl")).toString(),
                 QStringLiteral("http://sd:7860"));

        QCOMPARE(m_registry->activeProviderId(), QStringLiteral("automatic1111"));

        m_registry.reset();
        m_registry = std::make_unique<ImageProviderRegistry>(*m_settings);
        QCOMPARE(m_registry->list().size(), 2);
    }
};

QTEST_MAIN(TestImageProviderRegistry)
#include "test-image-provider-registry.moc"
