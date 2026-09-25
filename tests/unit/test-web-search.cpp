// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/db-manager.h"
#include "services/search/custom-json-provider.h"
#include "services/search/ddg-html-provider.h"
#include "services/search/ddg-instant-answer-provider.h"
#include "services/search/exa-provider.h"
#include "services/search/langsearch-provider.h"
#include "services/search/searxng-provider.h"
#include "services/search/tavily-provider.h"
#include "services/search/web-search-provider-registry.h"
#include "services/search/web-search-service.h"
#include "services/search/web-search-util.h"
#include "services/settings-service.h"

#include <QTemporaryDir>
#include <QtTest/QtTest>

#include <QHash>

using namespace Search;

class TestWebSearch : public QObject {
    Q_OBJECT

  private slots:
    void test_stripHtml_decodesEntitiesAndTags() {
        const QString in = QStringLiteral("<b>Hi</b> &amp; bye &#x27;x&#39; &lt;tag&gt; &#183;");
        const QString out = stripHtmlToText(in);
        QCOMPARE(out, QStringLiteral("Hi & bye 'x' <tag> ·"));
    }

    void test_stripHtml_collapsesWhitespace() {
        QCOMPARE(stripHtmlToText(QStringLiteral("  a\n   b\t c ")), QStringLiteral("a b c"));
    }

    void test_unwrap_redirect() {
        const QString red =
            QStringLiteral("//duckduckgo.com/l/?uddg=https%3A%2F%2Fexample.com%2Fp&rut=abc");
        QCOMPARE(unwrapDdgRedirect(red), QStringLiteral("https://example.com/p"));
    }

    void test_unwrap_passthrough_direct_url() {
        const QString direct = QStringLiteral("https://pypi.org/project/x/");
        QCOMPARE(unwrapDdgRedirect(direct), direct);
    }

    void test_ddgHtml_parsesRealResults() {
        const QByteArray html = QByteArrayLiteral(
            "<table>"
            "<tr><td>1.</td><td>"
            "<a rel=\"nofollow\" href=\"https://pypi.org/project/openssl-encrypt/\""
            " class='result-link'>openssl-encrypt &#183; PyPI</a>"
            "</td></tr>"
            "<tr><td>&nbsp;</td><td class='result-snippet'>"
            "OpenSSL Encrypt A <b>Python</b>-based file <b>encryption</b> "
            "<b>tool</b> for grandma&#x27;s recipes ..."
            "</td></tr>"
            "<tr><td>2.</td><td>"
            "<a rel=\"nofollow\" href=\"https://github.com/topics/encryption-tool\""
            " class='result-link'>encryption-tool &#183; GitHub</a>"
            "</td></tr>"
            "<tr><td>&nbsp;</td><td class='result-snippet'>"
            "A curated list of <b>encryption</b> projects."
            "</td></tr>"
            "</table>");

        const WebSearchResponse r =
            DdgHtmlProvider::parseLite(html, QStringLiteral("encryption"), 8);

        QCOMPARE(r.status, WebSearchStatus::Ok);
        QCOMPARE(r.results.size(), 2);

        QCOMPARE(r.results[0].url, QStringLiteral("https://pypi.org/project/openssl-encrypt/"));
        QVERIFY(r.results[0].title.contains(QStringLiteral("openssl-encrypt")));
        QVERIFY(r.results[0].title.contains(QStringLiteral("PyPI")));
        QVERIFY(!r.results[0].title.contains(QLatin1Char('<')));
        QVERIFY(!r.results[0].title.contains(QStringLiteral("&#")));
        QVERIFY(r.results[0].snippet.contains(QStringLiteral("Python-based")));
        QVERIFY(!r.results[0].snippet.contains(QStringLiteral("<b>")));
        QVERIFY(r.results[0].snippet.contains(QLatin1Char('\'')));

        QCOMPARE(r.results[1].url, QStringLiteral("https://github.com/topics/encryption-tool"));
    }

    void test_ddgHtml_respectsMaxResults() {
        const QByteArray html = QByteArrayLiteral(
            "<a rel=\"nofollow\" href=\"https://a.test/\" class='result-link'>A</a>"
            "<td class='result-snippet'>sa</td>"
            "<a rel=\"nofollow\" href=\"https://b.test/\" class='result-link'>B</a>"
            "<td class='result-snippet'>sb</td>");
        const WebSearchResponse r = DdgHtmlProvider::parseLite(html, QStringLiteral("q"), 1);
        QCOMPARE(r.results.size(), 1);
        QCOMPARE(r.status, WebSearchStatus::Ok);
    }

    void test_ddgHtml_emptyPageNoResults() {
        const WebSearchResponse r = DdgHtmlProvider::parseLite(
            QByteArrayLiteral("<html><body>nothing here</body></html>"), QStringLiteral("q"), 8);
        QCOMPARE(r.status, WebSearchStatus::NoResults);
        QVERIFY(r.results.isEmpty());
    }

    void test_instantAnswer_parsesAbstractAndAnswer() {
        const QByteArray json =
            QByteArrayLiteral("{\"Answer\":\"42\","
                              "\"AbstractText\":\"The answer to life.\","
                              "\"AbstractURL\":\"https://example.com/hhgttg\","
                              "\"Heading\":\"Hitchhiker\","
                              "\"RelatedTopics\":[{\"Text\":\"Towel - a useful item\","
                              "\"FirstURL\":\"https://example.com/towel\"}]}");
        const WebSearchResponse r =
            DdgInstantAnswerProvider::parseInstantAnswer(json, QStringLiteral("answer to life"), 8);
        QCOMPARE(r.status, WebSearchStatus::Ok);
        QCOMPARE(r.answer, QStringLiteral("42"));
        QVERIFY(r.results.size() >= 2);
        QCOMPARE(r.results[0].url, QStringLiteral("https://example.com/hhgttg"));
        QCOMPARE(r.results[0].title, QStringLiteral("Hitchhiker"));
        QCOMPARE(r.results[1].title, QStringLiteral("Towel"));
    }

    void test_instantAnswer_emptyObjectNoResults() {
        const WebSearchResponse r = DdgInstantAnswerProvider::parseInstantAnswer(
            QByteArrayLiteral("{}"), QStringLiteral("xyzzy"), 8);
        QCOMPARE(r.status, WebSearchStatus::NoResults);
        QVERIFY(r.results.isEmpty());
        QVERIFY(r.answer.isEmpty());
    }

    void test_instantAnswer_malformedJsonUnavailable() {
        const WebSearchResponse r = DdgInstantAnswerProvider::parseInstantAnswer(
            QByteArrayLiteral("{not json"), QStringLiteral("q"), 8);
        QCOMPARE(r.status, WebSearchStatus::Unavailable);
    }

    void test_service_emptyQueryIsGracefulNotError() {
        WebSearchService svc;
        const WebSearchResponse r = svc.search(QStringLiteral("   "));
        QCOMPARE(r.status, WebSearchStatus::Unavailable);
        QVERIFY(!r.note.isEmpty());
    }

    void test_service_defaultProviderIsDdgHtml() {
        WebSearchService svc;
        QCOMPARE(svc.activeConfig().providerId, QStringLiteral("ddg-html"));
    }

    void test_tavily_parsesResultsAndAnswer() {
        const QByteArray json =
            QByteArrayLiteral("{\"query\":\"q\",\"answer\":\"Messi is a footballer.\","
                              "\"results\":[{\"title\":\"Lionel Messi\","
                              "\"url\":\"https://en.wikipedia.org/wiki/Lionel_Messi\","
                              "\"content\":\"Argentine professional footballer.\","
                              "\"score\":0.98}]}");
        const WebSearchResponse r = TavilyProvider::parseTavily(json, QStringLiteral("q"), 8);
        QCOMPARE(r.status, WebSearchStatus::Ok);
        QCOMPARE(r.answer, QStringLiteral("Messi is a footballer."));
        QCOMPARE(r.results.size(), 1);
        QCOMPARE(r.results[0].title, QStringLiteral("Lionel Messi"));
        QCOMPARE(r.results[0].url, QStringLiteral("https://en.wikipedia.org/wiki/Lionel_Messi"));
        QVERIFY(r.results[0].snippet.contains(QStringLiteral("Argentine")));
    }

    void test_tavily_malformedUnavailable() {
        const WebSearchResponse r =
            TavilyProvider::parseTavily(QByteArrayLiteral("nope"), QStringLiteral("q"), 8);
        QCOMPARE(r.status, WebSearchStatus::Unavailable);
    }

    void test_exa_buildsSnippetFromHighlights() {
        const QByteArray json = QByteArrayLiteral("{\"results\":[{\"title\":\"LLM Overview\","
                                                  "\"url\":\"https://arxiv.org/abs/2307.06435\","
                                                  "\"highlights\":[\"limited their adoption\","
                                                  "\"comprehensive survey\"]}],"
                                                  "\"costDollars\":{\"total\":0.007}}");
        const WebSearchResponse r = ExaProvider::parseExa(json, QStringLiteral("llm"), 8);
        QCOMPARE(r.status, WebSearchStatus::Ok);
        QCOMPARE(r.results.size(), 1);
        QCOMPARE(r.results[0].url, QStringLiteral("https://arxiv.org/abs/2307.06435"));
        QVERIFY(r.results[0].snippet.contains(QStringLiteral("adoption")));
        QVERIFY(r.results[0].snippet.contains(QStringLiteral("survey")));
    }

    void test_exa_emptyResultsNoResults() {
        const WebSearchResponse r =
            ExaProvider::parseExa(QByteArrayLiteral("{\"results\":[]}"), QStringLiteral("q"), 8);
        QCOMPARE(r.status, WebSearchStatus::NoResults);
    }

    void test_langsearch_parsesNestedWebPages() {
        const QByteArray json =
            QByteArrayLiteral("{\"code\":200,\"data\":{\"webPages\":{\"value\":["
                              "{\"name\":\"Apple ESG\",\"url\":\"https://apple.com/esg\","
                              "\"snippet\":\"Environmental report 2024.\"}]}}}");
        const WebSearchResponse r =
            LangSearchProvider::parseLangSearch(json, QStringLiteral("esg"), 8);
        QCOMPARE(r.status, WebSearchStatus::Ok);
        QCOMPARE(r.results.size(), 1);
        QCOMPARE(r.results[0].title, QStringLiteral("Apple ESG"));
        QCOMPARE(r.results[0].url, QStringLiteral("https://apple.com/esg"));
        QVERIFY(r.results[0].snippet.contains(QStringLiteral("Environmental")));
    }

    void test_langsearch_emptyNoResults() {
        const WebSearchResponse r = LangSearchProvider::parseLangSearch(
            QByteArrayLiteral("{\"data\":{\"webPages\":{\"value\":[]}}}"), QStringLiteral("q"), 8);
        QCOMPARE(r.status, WebSearchStatus::NoResults);
    }

    void test_keyedProvider_missingKeyIsUnavailableNotCrash() {
        WebSearchConfig cfg;
        cfg.apiKey = QString();
        const WebSearchResponse r = TavilyProvider().search(cfg, QStringLiteral("q"));
        QCOMPARE(r.status, WebSearchStatus::Unavailable);
    }

    void test_service_providersCatalog() {
        WebSearchService svc;
        const QList<WebSearchProviderInfo> provs = svc.providers();
        QVERIFY(provs.size() >= 5);
        QHash<QString, bool> needsKey;
        for (const WebSearchProviderInfo& p : provs) {
            needsKey[p.id] = p.requiresApiKey;
        }
        QVERIFY(needsKey.contains(QStringLiteral("ddg-html")));
        QCOMPARE(needsKey.value(QStringLiteral("ddg-html")), false);
        QCOMPARE(needsKey.value(QStringLiteral("ddg-instant")), false);
        QCOMPARE(needsKey.value(QStringLiteral("tavily")), true);
        QCOMPARE(needsKey.value(QStringLiteral("exa")), true);
        QCOMPARE(needsKey.value(QStringLiteral("langsearch")), true);
        QVERIFY(needsKey.contains(QStringLiteral("searxng")));
        QCOMPARE(needsKey.value(QStringLiteral("searxng")), false);
        QVERIFY(needsKey.contains(QStringLiteral("custom")));
        QCOMPARE(needsKey.value(QStringLiteral("custom")), false);
    }

    void test_searxng_parsesResults() {
        const QByteArray json =
            QByteArrayLiteral("{\"query\":\"q\",\"results\":["
                              "{\"title\":\"Result One\",\"url\":\"https://one.test/\","
                              "\"content\":\"first snippet\"},"
                              "{\"title\":\"Result Two\",\"url\":\"https://two.test/\","
                              "\"content\":\"second snippet\"}]}");
        const WebSearchResponse r = SearxngProvider::parseSearxng(json, QStringLiteral("q"), 8);
        QCOMPARE(r.status, WebSearchStatus::Ok);
        QCOMPARE(r.results.size(), 2);
        QCOMPARE(r.results[0].url, QStringLiteral("https://one.test/"));
        QCOMPARE(r.results[0].snippet, QStringLiteral("first snippet"));
    }

    void test_searxng_missingBaseUrlIsUnavailable() {
        WebSearchConfig cfg;
        const WebSearchResponse r = SearxngProvider().search(cfg, QStringLiteral("q"));
        QCOMPARE(r.status, WebSearchStatus::Unavailable);
    }

    void test_probe_unknownProviderUnavailable() {
        WebSearchService svc;
        WebSearchConfig cfg;
        cfg.providerId = QStringLiteral("does-not-exist");
        const WebSearchResponse r = svc.probe(cfg, QStringLiteral("q"));
        QCOMPARE(r.status, WebSearchStatus::Unavailable);
    }

    void test_custom_nestedPathAndCustomKeys() {
        const QByteArray json = QByteArrayLiteral("{\"data\":{\"webPages\":{\"value\":["
                                                  "{\"name\":\"Doc A\",\"url\":\"https://a.test/\","
                                                  "\"snippet\":\"alpha\"},"
                                                  "{\"name\":\"Doc B\",\"url\":\"https://b.test/\","
                                                  "\"snippet\":\"beta\"}]}}}");
        const WebSearchResponse r =
            CustomJsonProvider::parseCustom(json,
                                            QStringLiteral("data.webPages.value"),
                                            QStringLiteral("name"),
                                            QStringLiteral("url"),
                                            QStringLiteral("snippet"),
                                            QStringLiteral("q"),
                                            8);
        QCOMPARE(r.status, WebSearchStatus::Ok);
        QCOMPARE(r.results.size(), 2);
        QCOMPARE(r.results[0].title, QStringLiteral("Doc A"));
        QCOMPARE(r.results[0].url, QStringLiteral("https://a.test/"));
        QCOMPARE(r.results[0].snippet, QStringLiteral("alpha"));
    }

    void test_custom_topLevelArrayEmptyPath() {
        const QByteArray json =
            QByteArrayLiteral("[{\"title\":\"One\",\"url\":\"https://one.test/\","
                              "\"content\":\"c1\"}]");
        const WebSearchResponse r = CustomJsonProvider::parseCustom(
            json, QString(), QString(), QString(), QString(), QStringLiteral("q"), 8);
        QCOMPARE(r.status, WebSearchStatus::Ok);
        QCOMPARE(r.results.size(), 1);
        QCOMPARE(r.results[0].url, QStringLiteral("https://one.test/"));
    }

    void test_custom_defaultKeys() {
        const QByteArray json =
            QByteArrayLiteral("{\"results\":[{\"title\":\"T\",\"url\":\"https://x.test/\","
                              "\"content\":\"snip\"}]}");
        const WebSearchResponse r = CustomJsonProvider::parseCustom(json,
                                                                    QStringLiteral("results"),
                                                                    QString(),
                                                                    QString(),
                                                                    QString(),
                                                                    QStringLiteral("q"),
                                                                    8);
        QCOMPARE(r.status, WebSearchStatus::Ok);
        QCOMPARE(r.results[0].title, QStringLiteral("T"));
        QCOMPARE(r.results[0].snippet, QStringLiteral("snip"));
    }

    void test_custom_malformedUnavailable() {
        const WebSearchResponse r = CustomJsonProvider::parseCustom(QByteArrayLiteral("not json"),
                                                                    QStringLiteral("results"),
                                                                    QString(),
                                                                    QString(),
                                                                    QString(),
                                                                    QStringLiteral("q"),
                                                                    8);
        QCOMPARE(r.status, WebSearchStatus::Unavailable);
    }

    void test_custom_noUrlConfiguredUnavailable() {
        WebSearchConfig cfg;
        cfg.providerId = QStringLiteral("custom");
        const WebSearchResponse r = CustomJsonProvider().search(cfg, QStringLiteral("q"));
        QCOMPARE(r.status, WebSearchStatus::Unavailable);
    }

    void test_probe_keyedNoKeyUnavailableNoFallback() {
        WebSearchService svc;
        WebSearchConfig cfg;
        cfg.providerId = QStringLiteral("tavily");
        const WebSearchResponse r = svc.probe(cfg, QStringLiteral("q"));
        QCOMPARE(r.status, WebSearchStatus::Unavailable);
        QCOMPARE(r.providerId, QStringLiteral("tavily"));
    }

    void test_registry_selectionDrivesService() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        DbManager::instance().close();
        QVERIFY(DbManager::instance().open(tmp.filePath(QStringLiteral("t.db"))));
        QVERIFY(DbManager::instance().runMigrations());

        SettingsService settings(DbManager::instance());
        WebSearchService svc;
        WebSearchProviderRegistry reg(svc, settings);

        QVERIFY(reg.providers().size() >= 5);
        QCOMPARE(reg.activeProviderId(), QStringLiteral("ddg-html"));

        reg.setActiveProvider(QStringLiteral("tavily"));
        QCOMPARE(reg.activeProviderId(), QStringLiteral("tavily"));
        QCOMPARE(svc.activeConfig().providerId, QStringLiteral("tavily"));

        reg.setBaseUrl(QStringLiteral("tavily"), QStringLiteral("https://proxy.test/search"));
        QCOMPARE(reg.baseUrl(QStringLiteral("tavily")),
                 QStringLiteral("https://proxy.test/search"));
        QCOMPARE(svc.activeConfig().baseUrl, QStringLiteral("https://proxy.test/search"));

        DbManager::instance().close();
    }
};

QTEST_MAIN(TestWebSearch)
#include "test-web-search.moc"
