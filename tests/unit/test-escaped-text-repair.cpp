// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "utils/escaped-text-repair.h"

#include <QtTest/QtTest>

class TestEscapedTextRepair : public QObject {
    Q_OBJECT

  private slots:
    void test_doubleEscapedDocument_repaired() {
        const QString corrupt = QStringLiteral("## Title\\n\\n**Bold intro**\\nParagraph one.\\n\\n"
                                               "- item a\\n- item b\\n\\nClosing line.");
        QVERIFY(Verzeta::looksDoubleEscaped(corrupt));

        const QString repaired = Verzeta::repairDoubleEscapedText(corrupt);
        QVERIFY(!repaired.contains(QStringLiteral("\\n")));
        QCOMPARE(repaired.count(QLatin1Char('\n')), 8);
        QVERIFY(repaired.startsWith(QStringLiteral("## Title\n\n**Bold")));
    }

    void test_multiLineCodeWithEscapes_untouched() {
        const QString code = QStringLiteral("int main() {\n"
                                            "    printf(\"a\\nb\\nc\\nd\");\n"
                                            "    return 0;\n"
                                            "}\n");
        QVERIFY(!Verzeta::looksDoubleEscaped(code));
        QCOMPARE(Verzeta::repairDoubleEscapedText(code), code);
    }

    void test_singleLineFewEscapes_untouched() {
        const QString oneLiner = QStringLiteral("printf(\"a\\nb\\nc\");");
        QVERIFY(!Verzeta::looksDoubleEscaped(oneLiner));
        QCOMPARE(Verzeta::repairDoubleEscapedText(oneLiner), oneLiner);
    }

    void test_minifiedJson_untouched() {
        const QString json = QStringLiteral("{\"a\":\"l1\\nl2\\nl3\",\"b\":\"x\\ny\\nz\",\"n\":1}");
        QVERIFY(!Verzeta::looksDoubleEscaped(json));
        QCOMPARE(Verzeta::repairDoubleEscapedText(json), json);
    }

    void test_windowsPairs_collapseCleanly() {
        const QString corrupt = QStringLiteral("line1\\r\\nline2\\r\\nline3\\r\\nline4");
        QVERIFY(Verzeta::looksDoubleEscaped(corrupt));
        const QString repaired = Verzeta::repairDoubleEscapedText(corrupt);
        QCOMPARE(repaired, QStringLiteral("line1\nline2\nline3\nline4"));
        QVERIFY(!repaired.contains(QStringLiteral("\\r")));
    }

    void test_trivialInputs_untouched() {
        QVERIFY(!Verzeta::looksDoubleEscaped(QString()));
        const QString plain = QStringLiteral("just one line, no escapes");
        QVERIFY(!Verzeta::looksDoubleEscaped(plain));
        QCOMPARE(Verzeta::repairDoubleEscapedText(plain), plain);
    }
};

QTEST_MAIN(TestEscapedTextRepair)
#include "test-escaped-text-repair.moc"
