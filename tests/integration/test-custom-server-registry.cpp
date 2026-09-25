// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/db-manager.h"
#include "services/custom-server-registry.h"
#include "services/model-router.h"
#include "services/settings-service.h"

#include <QTemporaryDir>
#include <QtTest>

#include <QDateTime>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QStandardPaths>

class TestCustomServerRegistry : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    QString m_dbPath;
    std::unique_ptr<SettingsService> m_settings;
    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<CustomServerRegistry> m_registry;

  private slots:

    void initTestCase() { QStandardPaths::setTestModeEnabled(true); }

    void cleanupTestCase() { QStandardPaths::setTestModeEnabled(false); }

    void init() {
        QVERIFY(m_tempDir.isValid());
        m_dbPath = m_tempDir.path() +
                   QStringLiteral("/cust_%1.db").arg(QDateTime::currentMSecsSinceEpoch());

        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(m_dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_settings = std::make_unique<SettingsService>(DbManager::instance());
        m_router = std::make_unique<ModelRouter>();
        m_registry = std::make_unique<CustomServerRegistry>(*m_router, *m_settings);
    }

    void cleanup() {
        m_registry.reset();
        m_router.reset();
        m_settings.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    }


    void test_add_returns_slug_and_persists() {
        QSignalSpy spy(m_registry.get(), &CustomServerRegistry::serversChanged);

        const QString slug = m_registry->add(QStringLiteral("LM Studio at home"),
                                             QStringLiteral("http://localhost:1234/v1"),
                                             false,
                                             QString(),
                                             true,
                                             true,
                                             false);

        QVERIFY(!slug.isEmpty());
        QCOMPARE(slug, QStringLiteral("lm-studio-at-home"));
        QCOMPARE(spy.count(), 1);

        ILLMProvider* p = m_router->providerForId(QStringLiteral("custom.lm-studio-at-home"));
        QVERIFY2(p, "provider not registered with ModelRouter");
        QCOMPARE(p->providerId(), QStringLiteral("custom.lm-studio-at-home"));
        QCOMPARE(p->displayName(), QStringLiteral("LM Studio at home"));

        const QString blob = m_settings->customServersBlob();
        QVERIFY(!blob.isEmpty());
        const QJsonDocument doc = QJsonDocument::fromJson(blob.toUtf8());
        QVERIFY(doc.isArray());
        QCOMPARE(doc.array().size(), 1);
        const QJsonObject row = doc.array().first().toObject();
        QCOMPARE(row.value(QStringLiteral("slug")).toString(), QStringLiteral("lm-studio-at-home"));
        QCOMPARE(row.value(QStringLiteral("baseUrl")).toString(),
                 QStringLiteral("http://localhost:1234/v1"));
        QCOMPARE(row.value(QStringLiteral("supportsStreaming")).toBool(), true);

        QVERIFY2(!row.contains(QStringLiteral("apiKey")), "apiKey leaked into the catalogue blob");
    }

    void test_add_rejects_empty_displayName() {
        const QString slug = m_registry->add(QString(),
                                             QStringLiteral("http://localhost:1234/v1"),
                                             false,
                                             QString(),
                                             true,
                                             true,
                                             false);
        QVERIFY(slug.isEmpty());
        QCOMPARE(m_registry->list().size(), 0);
    }

    void test_add_rejects_invalid_url() {
        const QString slug = m_registry->add(QStringLiteral("Bad URL"),
                                             QStringLiteral("not-a-url"),
                                             false,
                                             QString(),
                                             true,
                                             true,
                                             false);
        QVERIFY(slug.isEmpty());
        QCOMPARE(m_registry->list().size(), 0);
    }

    void test_add_collision_appends_suffix() {
        const QString first = m_registry->add(QStringLiteral("LM Studio"),
                                              QStringLiteral("http://a:1234/v1"),
                                              false,
                                              QString(),
                                              true,
                                              true,
                                              false);
        QCOMPARE(first, QStringLiteral("lm-studio"));
        const QString second = m_registry->add(QStringLiteral("LM Studio"),
                                               QStringLiteral("http://b:1234/v1"),
                                               false,
                                               QString(),
                                               true,
                                               true,
                                               false);
        QCOMPARE(second, QStringLiteral("lm-studio-2"));
        const QString third = m_registry->add(QStringLiteral("LM Studio"),
                                              QStringLiteral("http://c:1234/v1"),
                                              false,
                                              QString(),
                                              true,
                                              true,
                                              false);
        QCOMPARE(third, QStringLiteral("lm-studio-3"));
        QCOMPARE(m_registry->list().size(), 3);
    }

    void test_add_rejects_duplicate_url() {
        const QString first = m_registry->add(QStringLiteral("LM Studio"),
                                              QStringLiteral("http://localhost:1234/v1"),
                                              false,
                                              QString(),
                                              true,
                                              true,
                                              false);
        QVERIFY(!first.isEmpty());

        const QString dup = m_registry->add(QStringLiteral("LM Studio Home"),
                                            QStringLiteral("http://localhost:1234/v1"),
                                            false,
                                            QString(),
                                            true,
                                            true,
                                            false);
        QVERIFY2(dup.isEmpty(), "duplicate base URL must be refused");
        QCOMPARE(m_registry->list().size(), 1);

        QCOMPARE(m_registry->slugForBaseUrl(QStringLiteral("HTTP://LOCALHOST:1234/v1/")), first);
        QVERIFY(m_registry->slugForBaseUrl(QStringLiteral("http://other:9999/v1")).isEmpty());
    }

    void test_add_with_apiKey_persists_to_keychain_not_blob() {
        const QString slug = m_registry->add(QStringLiteral("Keyed Server"),
                                             QStringLiteral("http://localhost:9000/v1"),
                                             true,
                                             QStringLiteral("sk-test-1234"),
                                             true,
                                             true,
                                             false);
        QVERIFY(!slug.isEmpty());

        QCOMPARE(m_settings->apiKey(QStringLiteral("custom.keyed-server")),
                 QStringLiteral("sk-test-1234"));

        const QString blob = m_settings->customServersBlob();
        QVERIFY2(!blob.contains(QStringLiteral("sk-test-1234")),
                 "raw API key leaked into the persistent blob");
    }


    void test_byslug_returns_map_with_expected_keys() {
        m_registry->add(QStringLiteral("vLLM"),
                        QStringLiteral("http://localhost:8000/v1"),
                        false,
                        QString(),
                        true,
                        true,
                        false);
        const QVariantMap m = m_registry->byslug(QStringLiteral("vllm"));
        QVERIFY(!m.isEmpty());
        QCOMPARE(m.value(QStringLiteral("slug")).toString(), QStringLiteral("vllm"));
        QCOMPARE(m.value(QStringLiteral("displayName")).toString(), QStringLiteral("vLLM"));
        QCOMPARE(m.value(QStringLiteral("supportsStreaming")).toBool(), true);
        QCOMPARE(m.value(QStringLiteral("supportsToolCalling")).toBool(), true);
        QCOMPARE(m.value(QStringLiteral("supportsVision")).toBool(), false);
        QCOMPARE(m.value(QStringLiteral("requiresApiKeyFlag")).toBool(), false);
        QCOMPARE(m.value(QStringLiteral("hasApiKey")).toBool(), false);
    }

    void test_byslug_unknown_returns_empty() {
        const QVariantMap m = m_registry->byslug(QStringLiteral("nope"));
        QVERIFY(m.isEmpty());
    }


    void test_update_changes_propagate_to_provider() {
        const QString slug = m_registry->add(QStringLiteral("Server"),
                                             QStringLiteral("http://old:1234/v1"),
                                             false,
                                             QString(),
                                             true,
                                             true,
                                             false);

        QSignalSpy spy(m_registry.get(), &CustomServerRegistry::serversChanged);

        const bool ok = m_registry->update(slug,
                                           QStringLiteral("Server Renamed"),
                                           QStringLiteral("http://new:5678/v1"),
                                           true,
                                           QStringLiteral("new-key"),
                                           false,
                                           false,
                                           true);
        QVERIFY(ok);
        QCOMPARE(spy.count(), 1);

        ILLMProvider* p = m_router->providerForId(QStringLiteral("custom.server"));
        QVERIFY(p);
        QCOMPARE(p->displayName(), QStringLiteral("Server Renamed"));
        QVERIFY(!p->supportsStreaming());
        QVERIFY(!p->supportsToolCalling());
        QVERIFY(p->supportsVision());

        QCOMPARE(m_settings->apiKey(QStringLiteral("custom.server")), QStringLiteral("new-key"));

        const QString blob = m_settings->customServersBlob();
        QVERIFY(blob.contains(QStringLiteral("Server Renamed")));
        QVERIFY(blob.contains(QStringLiteral("http://new:5678/v1")));
    }

    void test_update_unknown_slug_returns_false() {
        const bool ok = m_registry->update(QStringLiteral("ghost"),
                                           QStringLiteral("Name"),
                                           QStringLiteral("http://x:1/v1"),
                                           false,
                                           QString(),
                                           true,
                                           true,
                                           false);
        QVERIFY(!ok);
    }


    void test_remove_unregisters_and_persists() {
        const QString slug = m_registry->add(QStringLiteral("To Delete"),
                                             QStringLiteral("http://x:9/v1"),
                                             true,
                                             QStringLiteral("k"),
                                             true,
                                             true,
                                             false);
        QVERIFY(m_router->providerForId(QStringLiteral("custom.to-delete")) != nullptr);

        QSignalSpy spy(m_registry.get(), &CustomServerRegistry::serversChanged);

        const bool ok = m_registry->remove(slug);
        QVERIFY(ok);
        QCOMPARE(spy.count(), 1);

        QVERIFY(m_router->providerForId(QStringLiteral("custom.to-delete")) == nullptr);

        const QString blob = m_settings->customServersBlob();
        QVERIFY(!blob.contains(QStringLiteral("to-delete")));
        QCOMPARE(m_settings->apiKey(QStringLiteral("custom.to-delete")), QString());
    }

    void test_remove_unknown_returns_false() {
        QVERIFY(!m_registry->remove(QStringLiteral("ghost")));
    }


    void test_roundtrip_hydrate_restores_all_rows() {
        m_registry->add(QStringLiteral("Alpha"),
                        QStringLiteral("http://alpha:1/v1"),
                        true,
                        QStringLiteral("alpha-key"),
                        true,
                        false,
                        true);
        m_registry->add(QStringLiteral("Beta"),
                        QStringLiteral("http://beta:2/v1"),
                        false,
                        QString(),
                        false,
                        true,
                        false);

        m_registry.reset();
        m_router.reset();
        m_router = std::make_unique<ModelRouter>();
        m_registry = std::make_unique<CustomServerRegistry>(*m_router, *m_settings);

        const QVariantList list = m_registry->list();
        QCOMPARE(list.size(), 2);

        const QVariantMap alpha = m_registry->byslug(QStringLiteral("alpha"));
        QVERIFY(!alpha.isEmpty());
        QCOMPARE(alpha.value(QStringLiteral("baseUrl")).toString(),
                 QStringLiteral("http://alpha:1/v1"));
        QCOMPARE(alpha.value(QStringLiteral("requiresApiKeyFlag")).toBool(), true);
        QCOMPARE(alpha.value(QStringLiteral("supportsStreaming")).toBool(), true);
        QCOMPARE(alpha.value(QStringLiteral("supportsToolCalling")).toBool(), false);
        QCOMPARE(alpha.value(QStringLiteral("supportsVision")).toBool(), true);
        QCOMPARE(alpha.value(QStringLiteral("hasApiKey")).toBool(), true);

        const QVariantMap beta = m_registry->byslug(QStringLiteral("beta"));
        QVERIFY(!beta.isEmpty());
        QCOMPARE(beta.value(QStringLiteral("supportsStreaming")).toBool(), false);
        QCOMPARE(beta.value(QStringLiteral("supportsToolCalling")).toBool(), true);
        QCOMPARE(beta.value(QStringLiteral("hasApiKey")).toBool(), false);

        QVERIFY(m_router->providerForId(QStringLiteral("custom.alpha")) != nullptr);
        QVERIFY(m_router->providerForId(QStringLiteral("custom.beta")) != nullptr);

        QCOMPARE(m_settings->apiKey(QStringLiteral("custom.alpha")), QStringLiteral("alpha-key"));
        QCOMPARE(m_settings->apiKey(QStringLiteral("custom.beta")), QString());
    }
};

QTEST_MAIN(TestCustomServerRegistry)
#include "test-custom-server-registry.moc"
