// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "services/search/web-search-service.h"
#include "tools/web/search-web-tool.h"

#include <QtTest/QtTest>

class TestSearchWebTool : public QObject {
    Q_OBJECT

  private slots:
    void test_contract() {
        Search::WebSearchService svc;
        Tools::SearchWebTool tool(svc);
        QCOMPARE(tool.name(), QStringLiteral("search_web"));
        QVERIFY(!tool.description().isEmpty());
        QCOMPARE(tool.runsOnMainThread(), false);
    }

    void test_parameters_queryRequiredString() {
        Search::WebSearchService svc;
        Tools::SearchWebTool tool(svc);
        const auto params = tool.parameters();
        QCOMPARE(params.size(), 1);
        QCOMPARE(params.first().name, QStringLiteral("query"));
        QCOMPARE(params.first().type, QStringLiteral("string"));
        QCOMPARE(params.first().required, true);
    }

    void test_invoke_emptyQuery_returnsErrorWithoutNetwork() {
        Search::WebSearchService svc;
        Tools::SearchWebTool tool(svc);
        QJsonObject args;
        args[QStringLiteral("query")] = QString();
        const QJsonValue result = tool.invoke(args);
        QVERIFY(result.isObject());
        QVERIFY(result.toObject().contains(QStringLiteral("error")));
    }
};

QTEST_MAIN(TestSearchWebTool)
#include "test-search-web-tool.moc"
