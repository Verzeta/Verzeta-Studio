// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "tools/cascade/request-turn-tool.h"

#include <QtTest/QtTest>

class TestRequestTurnTool : public QObject {
    Q_OBJECT

  private slots:
    void test_contract_nameAndThreadResidency() {
        Tools::RequestTurnTool tool;
        QCOMPARE(tool.name(), QStringLiteral("request_turn"));
        QCOMPARE(tool.runsOnMainThread(), false);
    }

    void test_parameters_aliasRequiredContextOptional() {
        Tools::RequestTurnTool tool;
        const auto params = tool.parameters();
        QCOMPARE(params.size(), 2);
        QCOMPARE(params[0].name, QStringLiteral("alias"));
        QCOMPARE(params[0].type, QStringLiteral("string"));
        QCOMPARE(params[0].required, true);
        QCOMPARE(params[1].name, QStringLiteral("context"));
        QCOMPARE(params[1].type, QStringLiteral("string"));
        QCOMPARE(params[1].required, false);
    }

    void test_invoke_validAlias_returnsQueued() {
        Tools::RequestTurnTool tool;

        QJsonObject args;
        args[QStringLiteral("alias")] = QStringLiteral("Alice");

        const QJsonValue result = tool.invoke(args);
        QVERIFY(result.isObject());
        const QJsonObject obj = result.toObject();
        QCOMPARE(obj[QStringLiteral("requested")].toString(), QStringLiteral("Alice"));
        QCOMPARE(obj[QStringLiteral("status")].toString(), QStringLiteral("queued"));
    }

    void test_invoke_emptyAlias_returnsError() {
        Tools::RequestTurnTool tool;

        QJsonObject args;
        args[QStringLiteral("alias")] = QString();

        const QJsonValue result = tool.invoke(args);
        QVERIFY(result.isObject());
        QVERIFY(result.toObject().contains(QStringLiteral("error")));
    }

    void test_invoke_broadcastAlias_preservedByteIdentical() {
        Tools::RequestTurnTool tool;

        QJsonObject args;
        args[QStringLiteral("alias")] = QStringLiteral("all");

        const QJsonValue result = tool.invoke(args);
        QCOMPARE(result.toObject()[QStringLiteral("requested")].toString(), QStringLiteral("all"));
    }
};

QTEST_MAIN(TestRequestTurnTool)
#include "test-request-turn-tool.moc"
