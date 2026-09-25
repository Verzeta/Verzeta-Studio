// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/db-manager.h"
#include "services/conversation-service.h"
#include "services/plan-service.h"
#include "services/task-observer.h"

#include <QTemporaryDir>
#include <QTest>

#include <QDateTime>
#include <QSqlDatabase>

class TestTaskObserver : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<PlanService> m_planSvc;
    std::unique_ptr<TaskObserver> m_observer;
    QString m_convId;
    QString m_planId;
    QString m_stepId;

    QString createPlanWithOneStep() {
        AgentPlan plan;
        plan.conversationId = m_convId;
        plan.goal = QStringLiteral("do the thing");
        plan.startedBy = QStringLiteral("user");
        plan.status = PlanStatus::Executing;

        PlanStep s;
        s.title = QStringLiteral("Step 1");
        s.description = QStringLiteral("desc");
        s.ownerAlias = QStringLiteral("alice");
        s.acceptanceCriteria = QStringLiteral("do it");
        s.status = StepStatus::InProgress;

        return m_planSvc->createPlan(plan, {s});
    }

  private slots:

    void init() {
        const QString dbPath =
            m_tempDir.path() +
            QStringLiteral("/test_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_convSvc = std::make_unique<ConversationService>(DbManager::instance());
        m_planSvc = std::make_unique<PlanService>(DbManager::instance());
        m_observer = std::make_unique<TaskObserver>(*m_planSvc);

        m_convId = m_convSvc->createConversation(QStringLiteral("Chat"));
        QVERIFY(!m_convId.isEmpty());
        m_planId = createPlanWithOneStep();
        QVERIFY(!m_planId.isEmpty());
        const auto steps = m_planSvc->stepsForPlan(m_planId);
        QCOMPARE(steps.size(), 1);
        m_stepId = steps.first().id;
    }

    void cleanup() {
        m_observer.reset();
        m_planSvc.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    }

    void test_recordToolCall_withInvoker_usesThatAlias() {
        const QString id = m_observer->recordToolCall(m_planId,
                                                      QStringLiteral("write_file"),
                                                      QStringLiteral("{\"path\":\"/tmp/x\"}"),
                                                      QStringLiteral("ok"),
                                                      QString{},
                                                      QStringLiteral("Writer_Rob"));
        QVERIFY(!id.isEmpty());

        const QList<StepArtifact> arts = m_planSvc->artifactsForStep(m_stepId);
        QCOMPARE(arts.size(), 1);
        QCOMPARE(arts.first().submittedByAlias, QStringLiteral("Writer_Rob"));
        QCOMPARE(arts.first().type, ArtifactType::ToolLog);
    }

    void test_recordToolCall_withoutInvoker_fallsBackToSystem() {
        const QString id = m_observer->recordToolCall(m_planId,
                                                      QStringLiteral("run_shell"),
                                                      QStringLiteral("{\"cmd\":\"ls\"}"),
                                                      QStringLiteral("ok: 3 files"));
        QVERIFY(!id.isEmpty());

        const QList<StepArtifact> arts = m_planSvc->artifactsForStep(m_stepId);
        QCOMPARE(arts.size(), 1);
        QCOMPARE(arts.first().submittedByAlias, QStringLiteral("system"));
    }

    void test_recordFile_withoutAlias_fallsBackToSystem() {
        const QString id = m_observer->recordFile(
            m_planId, QStringLiteral("/tmp/out.txt"), QString{}, QStringLiteral("write_file"));
        QVERIFY(!id.isEmpty());

        const QList<StepArtifact> arts = m_planSvc->artifactsForStep(m_stepId);
        QCOMPARE(arts.size(), 1);
        QCOMPARE(arts.first().submittedByAlias, QStringLiteral("system"));
    }

    void test_recordTextArtifact_withoutAlias_fallsBackToSystem() {
        const QString id = m_observer->recordTextArtifact(m_planId,
                                                          QString{},
                                                          QStringLiteral("the quick brown fox..."),
                                                          QStringLiteral("story draft"));
        QVERIFY(!id.isEmpty());

        const QList<StepArtifact> arts = m_planSvc->artifactsForStep(m_stepId);
        QCOMPARE(arts.size(), 1);
        QCOMPARE(arts.first().submittedByAlias, QStringLiteral("system"));
        QCOMPARE(arts.first().type, ArtifactType::Draft);
    }

    void test_recordToolCall_unknownPlan_returnsEmpty() {
        const QString id = m_observer->recordToolCall(
            QStringLiteral("nope"), QStringLiteral("write_file"), QString{}, QStringLiteral("ok"));
        QVERIFY(id.isEmpty());
    }

    void test_recentActivity_returnsWrittenRows() {
        m_observer->recordToolCall(m_planId,
                                   QStringLiteral("search_web"),
                                   QStringLiteral("{\"q\":\"foo\"}"),
                                   QStringLiteral("ok: 3 results"),
                                   QString{},
                                   QStringLiteral("Researcher"));
        m_observer->recordFile(m_planId,
                               QStringLiteral("/tmp/out.txt"),
                               QStringLiteral("Writer"),
                               QStringLiteral("write_file"));
        m_observer->recordTextArtifact(m_planId,
                                       QStringLiteral("Writer"),
                                       QStringLiteral("a decent reply"),
                                       QStringLiteral("summary line"));

        const QStringList activity = m_observer->recentActivity(m_planId, 10);
        QCOMPARE(activity.size(), 3);
        bool hasDraft = false, hasFile = false, hasTool = false;
        for (const QString& line : activity) {
            if (line.startsWith(QStringLiteral("draft:")))
                hasDraft = true;
            if (line.startsWith(QStringLiteral("file:")))
                hasFile = true;
            if (line.startsWith(QStringLiteral("tool:")))
                hasTool = true;
        }
        QVERIFY(hasDraft);
        QVERIFY(hasFile);
        QVERIFY(hasTool);
    }
};

QTEST_MAIN(TestTaskObserver)
#include "test-task-observer.moc"
