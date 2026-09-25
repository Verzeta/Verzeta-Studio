// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "services/chat/tool-dispatcher.h"

#include <QTest>

using Chat::ToolDispatcher;

class TestClaimsDetector : public QObject {
    Q_OBJECT

  private:
    static QStringList toolNames() {
        return {QStringLiteral("write_file"),
                QStringLiteral("read_file"),
                QStringLiteral("edit_canvas"),
                QStringLiteral("open_canvas"),
                QStringLiteral("read_canvas"),
                QStringLiteral("search_web"),
                QStringLiteral("run_shell"),
                QStringLiteral("list_files")};
    }

    static QString gate(const QString& s) {
        return ToolDispatcher::narrationDeferredActionKind(s, toolNames());
    }

  private slots:
    void deferred_toolNameInProse() {
        QCOMPARE(gate(QStringLiteral("I'll use the edit_canvas tool for this.")),
                 QStringLiteral("deferred_action"));
    }

    void deferred_bareToolNameMention() {
        QCOMPARE(gate(QStringLiteral("Calling tool write_file…")),
                 QStringLiteral("deferred_action"));
    }

    void deferred_filenameToken_english() {
        QCOMPARE(gate(QStringLiteral("Next I will put everything into index.html.")),
                 QStringLiteral("deferred_action"));
    }

    void deferred_filenameToken_german() {
        QCOMPARE(gate(QStringLiteral("Ich erstelle jetzt die Datei styles.css und "
                                     "melde mich danach.")),
                 QStringLiteral("deferred_action"));
    }

    void deferred_filenameToken_withPath() {
        QCOMPARE(gate(QStringLiteral("docs/guide.md needs the new section.")),
                 QStringLiteral("deferred_action"));
    }

    void deferred_canvasSurface() {
        QCOMPARE(gate(QStringLiteral("Let me open this in the Canvas so we can "
                                     "iterate together.")),
                 QStringLiteral("deferred_action"));
    }

    void deferred_danglingPayloadColon() {
        QVERIFY(!gate(QStringLiteral("Tone locked. Here is the initial copy draft:")).isEmpty());
        QVERIFY(!gate(QStringLiteral("Der Ton steht. Hier ist der erste Entwurf:  \n")).isEmpty());
        QVERIFY(!gate(QStringLiteral("草稿如下：")).isEmpty());
    }

    void negative_colonMidReplyWithPayloadPresent() {
        QVERIFY(gate(QStringLiteral("Key points: tone, audience, and pricing. All are "
                                    "covered in the summary above."))
                    .isEmpty());
    }

    void negative_projectComplete() {
        QVERIFY(gate(QStringLiteral("Project Complete! Everything is delivered.")).isEmpty());
    }

    void negative_actionButNoConcreteToken() {
        QVERIFY(gate(QStringLiteral("I'll create the report and share it here.")).isEmpty());
    }

    void negative_versionNumberNotFilename() {
        QVERIFY(gate(QStringLiteral("Let's ship v0.0.1 tomorrow — 2.5 is too far out.")).isEmpty());
    }

    void negative_abbreviationsAndDomains() {
        QVERIFY(gate(QStringLiteral("Check example.com for inspiration, e.g. the hero "
                                    "section, i.e. the top banner."))
                    .isEmpty());
    }

    void negative_emptyAndWhitespace() {
        QVERIFY(gate(QString()).isEmpty());
        QVERIFY(gate(QStringLiteral("   \n  ")).isEmpty());
    }

    void negative_emptyToolListDisablesToolPathOnly() {
        QVERIFY(ToolDispatcher::narrationDeferredActionKind(
                    QStringLiteral("I'll use the frobnicate tool."), {})
                    .isEmpty());
        QCOMPARE(
            ToolDispatcher::narrationDeferredActionKind(QStringLiteral("Time to fill app.py."), {}),
            QStringLiteral("deferred_action"));
    }

    void targets_extractToolNamesAndFileBasenames() {
        const QSet<QString> t = ToolDispatcher::announcedActionTargets(
            QStringLiteral("I'll run search_web first, then finish "
                           "docs/Guide.MD and update the canvas."),
            toolNames());
        QVERIFY(t.contains(QStringLiteral("search_web")));
        QVERIFY(t.contains(QStringLiteral("guide.md")));
        QVERIFY(t.contains(QStringLiteral("canvas")));
    }

    void targets_emptyForPureDiscussion() {
        QVERIFY(ToolDispatcher::announcedActionTargets(
                    QStringLiteral("Great teamwork everyone, the plan "
                                   "looks solid."),
                    toolNames())
                    .isEmpty());
    }

    void fireAware_postBatchSummary_suppressed() {
        QVERIFY(ToolDispatcher::announcesOnlyExecutedWork(
            QStringLiteral("I've updated guide.md in the canvas."),
            toolNames(),
            {QStringLiteral("edit_canvas")},
            {QStringLiteral("guide.md")}));
    }

    void fireAware_newFileAnnounced_staysCandidate() {
        QVERIFY(!ToolDispatcher::announcesOnlyExecutedWork(
            QStringLiteral("styles.css is written; next I take "
                           "index.html."),
            toolNames(),
            {QStringLiteral("write_file")},
            {QStringLiteral("styles.css")}));
    }

    void fireAware_canvasCoveredByAnyCanvasCall() {
        QVERIFY(ToolDispatcher::announcesOnlyExecutedWork(
            QStringLiteral("I'm going to update the canvas now."),
            toolNames(),
            {QStringLiteral("edit_canvas")},
            {}));
        QVERIFY(!ToolDispatcher::announcesOnlyExecutedWork(
            QStringLiteral("I'm going to update the canvas now."),
            toolNames(),
            {QStringLiteral("search_web")},
            {}));
    }

    void fireAware_noTargets_backedByAnyExecution() {
        QVERIFY(ToolDispatcher::announcesOnlyExecutedWork(QStringLiteral("All done on my side."),
                                                          toolNames(),
                                                          {QStringLiteral("write_file")},
                                                          {}));
        QVERIFY(!ToolDispatcher::announcesOnlyExecutedWork(
            QStringLiteral("All done on my side."), toolNames(), {}, {}));
    }
};

QTEST_MAIN(TestClaimsDetector)
#include "test-claims-detector.moc"
