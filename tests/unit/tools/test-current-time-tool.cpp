// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "tools/time/current-time-tool.h"

#include <QtTest/QtTest>

#include <QDateTime>

class TestCurrentTimeTool : public QObject {
    Q_OBJECT

  private slots:
    void test_contract_nameDescriptionThreadResidency() {
        Tools::CurrentTimeTool tool;

        QCOMPARE(tool.name(), QStringLiteral("get_current_time"));
        QVERIFY(!tool.description().isEmpty());
        QCOMPARE(tool.runsOnMainThread(), false);
    }

    void test_parameters_empty() {
        Tools::CurrentTimeTool tool;
        QVERIFY(tool.parameters().isEmpty());
    }

    void test_invoke_returnsDatetimeAndTimezone() {
        Tools::CurrentTimeTool tool;

        const QJsonValue result = tool.invoke({});
        QVERIFY(result.isObject());
        const QJsonObject obj = result.toObject();

        QVERIFY(obj.contains(QStringLiteral("datetime")));
        QVERIFY(obj.contains(QStringLiteral("timezone")));

        const QString iso = obj[QStringLiteral("datetime")].toString();
        QVERIFY(!iso.isEmpty());
        QVERIFY(QDateTime::fromString(iso, Qt::ISODate).isValid());

        const QString tz = obj[QStringLiteral("timezone")].toString();
        QVERIFY(!tz.isEmpty());
    }
};

QTEST_MAIN(TestCurrentTimeTool)
#include "test-current-time-tool.moc"
