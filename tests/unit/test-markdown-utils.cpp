// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "utils/markdown-utils.h"

#include <QTest>

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QVariantList>
#include <QVariantMap>


class TestMarkdownUtils : public QObject {
    Q_OBJECT

  private:
    MarkdownConverter* m_converter = nullptr;

  private slots:

    void init() { m_converter = &MarkdownConverter::instance(); }

    void test_basicFormatting_boldAndItalic() {
        const QString html = m_converter->toHtml(QStringLiteral("**bold** and *italic*"));

        const bool hasBold = html.contains(QStringLiteral("<strong>")) ||
                             html.contains(QStringLiteral("<b>")) ||
                             html.contains(QStringLiteral("font-weight:600")) ||
                             html.contains(QStringLiteral("font-weight: 600")) ||
                             html.contains(QStringLiteral("font-weight:700")) ||
                             html.contains(QStringLiteral("font-weight: 700"));
        QVERIFY2(hasBold, qPrintable(QStringLiteral("Expected bold in HTML: ") + html.left(400)));

        const bool hasItalic = html.contains(QStringLiteral("<em>")) ||
                               html.contains(QStringLiteral("<i>")) ||
                               html.contains(QStringLiteral("font-style:italic")) ||
                               html.contains(QStringLiteral("font-style: italic"));
        QVERIFY2(hasItalic,
                 qPrintable(QStringLiteral("Expected italic in HTML: ") + html.left(400)));
    }

    void test_codeFences_producesPreCode() {
        const QString md = QStringLiteral("```\nprint('hello')\n```");
        const QString html = m_converter->toHtml(md);
        QVERIFY2(html.contains(QStringLiteral("<pre>")) || html.contains(QStringLiteral("<pre ")),
                 "Expected <pre> tag for code fence");
    }

    void test_languageDetection_splitSegmentsHaveLanguage() {
        const QString md = QStringLiteral("```python\nprint('hi')\n```");
        const QVariantList segs = m_converter->splitContentSegments(md);

        bool foundCodeWithPython = false;
        for (const QVariant& v : segs) {
            const QVariantMap m = v.toMap();
            if (m[QStringLiteral("type")].toString() == QStringLiteral("code") &&
                m[QStringLiteral("language")].toString() == QStringLiteral("python")) {
                foundCodeWithPython = true;
            }
        }
        QVERIFY2(foundCodeWithPython, "Expected a code segment with language 'python'");
    }

    void test_nestedFormatting_linkProducesAnchorTag() {
        const QString md = QStringLiteral("- [Click me](https://example.com)");
        const QString html = m_converter->toHtml(md);
        QVERIFY2(html.contains(QStringLiteral("href")),
                 "Expected href attribute for Markdown link");
        QVERIFY2(html.contains(QStringLiteral("example.com")), "Expected URL preserved in output");
    }

    void test_xssPrevention_scriptTagStripped() {
        const QString md = QStringLiteral("Hello <script>alert(1)</script> world");
        const QString html = m_converter->toHtml(md);
        QVERIFY2(!html.contains(QStringLiteral("<script")),
                 "XSS: <script> tag must not appear in sanitized output");
        QVERIFY2(!html.contains(QStringLiteral("alert(1)")),
                 "XSS: script content must not appear in sanitized output");
    }

    void test_eventHandlerStripping_onErrorRemoved() {
        const QString md = QStringLiteral("text <img src=\"x\" onerror=\"alert(1)\"> more");
        const QString html = m_converter->toHtml(md);
        QVERIFY2(!html.contains(QStringLiteral("onerror")),
                 "XSS: onerror event handler must be stripped");
    }

    void test_splitContentSegments_mixedContent() {
        const QString md =
            QStringLiteral("Some text before\n\n```cpp\nint x = 0;\n```\n\nSome text after");

        const QVariantList segs = m_converter->splitContentSegments(md);

        bool hasText = false;
        bool hasCode = false;
        for (const QVariant& v : segs) {
            const QVariantMap m = v.toMap();
            const QString type = m[QStringLiteral("type")].toString();
            if (type == QStringLiteral("text"))
                hasText = true;
            if (type == QStringLiteral("code"))
                hasCode = true;
        }
        QVERIFY2(hasText, "Expected at least one text segment");
        QVERIFY2(hasCode, "Expected at least one code segment");

        for (const QVariant& v : segs) {
            const QVariantMap m = v.toMap();
            if (m[QStringLiteral("type")].toString() == QStringLiteral("code")) {
                QCOMPARE(m[QStringLiteral("language")].toString(), QStringLiteral("cpp"));
                QVERIFY(
                    m[QStringLiteral("content")].toString().contains(QStringLiteral("int x = 0;")));
                break;
            }
        }
    }

    void test_extractCodeBlocks_jsonArrayCorrect() {
        const QString md = QStringLiteral("Before\n\n```python\nprint('hello')\n```\n\nAfter");

        const QString json = m_converter->extractCodeBlocks(md);
        QVERIFY(!json.isEmpty());

        const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
        QVERIFY(doc.isArray());

        const QJsonArray arr = doc.array();
        QVERIFY(arr.size() >= 1);

        const QJsonObject block = arr.at(0).toObject();
        QCOMPARE(block[QStringLiteral("language")].toString(), QStringLiteral("python"));
        QVERIFY(
            block[QStringLiteral("code")].toString().contains(QStringLiteral("print('hello')")));
    }

    void test_emptyInput_returnsEmpty() {
        QCOMPARE(m_converter->toHtml(QString{}), QString{});
        QCOMPARE(m_converter->extractCodeBlocks(QString{}), QStringLiteral("[]"));
        QCOMPARE(m_converter->splitContentSegments(QString{}).size(), 0);
    }

    void test_splitContentSegments_plainText_singleTextSegment() {
        const QString md = QStringLiteral("Just plain text with no code.");
        const QVariantList segs = m_converter->splitContentSegments(md);

        QCOMPARE(segs.size(), 1);
        const QVariantMap seg = segs.at(0).toMap();
        QCOMPARE(seg[QStringLiteral("type")].toString(), QStringLiteral("text"));
        QVERIFY(seg[QStringLiteral("content")].toString().contains(QStringLiteral("plain text")));
        QCOMPARE(seg[QStringLiteral("language")].toString(), QString{});
    }

    void test_splitContentSegments_unterminatedFence_splitsAsCode() {
        const QString md = QStringLiteral("Here is the content I intend to write:\n\n"
                                          "```markdown\n# Title\n\n- item one\n- item two\n");
        const QVariantList segs = m_converter->splitContentSegments(md);

        QCOMPARE(segs.size(), 2);
        const QVariantMap text = segs.at(0).toMap();
        QCOMPARE(text[QStringLiteral("type")].toString(), QStringLiteral("text"));
        QVERIFY(
            text[QStringLiteral("content")].toString().contains(QStringLiteral("intend to write")));

        const QVariantMap code = segs.at(1).toMap();
        QCOMPARE(code[QStringLiteral("type")].toString(), QStringLiteral("code"));
        QCOMPARE(code[QStringLiteral("language")].toString(), QStringLiteral("markdown"));
        QVERIFY(code[QStringLiteral("content")].toString().contains(QStringLiteral("# Title")));
        QVERIFY(code[QStringLiteral("content")].toString().contains(QStringLiteral("- item two")));
    }

    void test_latex_inlineArrowConverted() {
        const QString html =
            m_converter->toHtml(QStringLiteral("Draft $\\rightarrow$ Review $\\rightarrow$ Done"));
        QVERIFY(html.contains(QString::fromUtf8("→")));
        QVERIFY(!html.contains(QStringLiteral("rightarrow")));
        QVERIFY(!html.contains(QLatin1Char('$')));
    }

    void test_latex_symbolsAndGreek() {
        const QString html =
            m_converter->toHtml(QStringLiteral("\\alpha \\leq \\beta \\times \\gamma"));
        QVERIFY(html.contains(QString::fromUtf8("α")));
        QVERIFY(html.contains(QString::fromUtf8("≤")));
        QVERIFY(html.contains(QString::fromUtf8("β")));
        QVERIFY(html.contains(QString::fromUtf8("×")));
        QVERIFY(!html.contains(QStringLiteral("alpha")));
    }

    void test_latex_currencyNotTouched() {
        const QString html = m_converter->toHtml(QStringLiteral("It costs $5 and then $10 total."));
        QVERIFY(html.contains(QStringLiteral("$5")));
        QVERIFY(html.contains(QStringLiteral("$10")));
    }

    void test_latex_unknownCommandLeftVerbatim() {
        const QString html = m_converter->toHtml(QStringLiteral("Open C:\\Users then run it."));
        QVERIFY(html.contains(QStringLiteral("Users")));
    }

    void test_latex_codeFenceUntouched() {
        const QVariantList segs =
            m_converter->splitContentSegments(QStringLiteral("text\n```\nx = \\rightarrow\n```\n"));
        bool foundCode = false;
        for (const QVariant& v : segs) {
            const QVariantMap seg = v.toMap();
            if (seg[QStringLiteral("type")].toString() == QStringLiteral("code")) {
                foundCode = true;
                QVERIFY(seg[QStringLiteral("content")].toString().contains(
                    QStringLiteral("\\rightarrow")));
            }
        }
        QVERIFY(foundCode);
    }
};

QTEST_MAIN(TestMarkdownUtils)
#include "test-markdown-utils.moc"
