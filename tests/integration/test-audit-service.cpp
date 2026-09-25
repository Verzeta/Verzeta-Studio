// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/models/activity-event.h"
#include "../../backend/models/db-manager.h"
#include "../../backend/services/audit-service.h"

#include <QTemporaryDir>
#include <QtTest>

#include <memory>
#include <QJsonObject>
#include <QJsonValue>
#include <QSet>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QVariantList>
#include <QVariantMap>

class TestAuditService : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_dir;
    QString m_dbPath;
    std::unique_ptr<AuditService> m_audit;

  private slots:
    void init() {
        QVERIFY(m_dir.isValid());
        m_dbPath =
            m_dir.path() + QStringLiteral("/test_%1.db").arg(QDateTime::currentMSecsSinceEpoch());

        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(m_dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_audit = std::make_unique<AuditService>(DbManager::instance());
    }

    void cleanup() {
        m_audit.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    }

    void test_record_invalidEvent_doesNotWrite() {
        QSignalSpy spy(m_audit.get(), &AuditService::activityLogged);
        QVERIFY(spy.isValid());

        ActivityEvent bad;
        QVERIFY(!bad.isValid());

        m_audit->record(bad);

        QCOMPARE(spy.count(), 0);

        QSqlQuery q(DbManager::instance().db());
        QVERIFY(q.exec(QStringLiteral("SELECT COUNT(*) FROM activity_log")));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 0);
    }

    void test_record_validEvent_writesAndEmitsSignal() {
        QSignalSpy spy(m_audit.get(), &AuditService::activityLogged);
        QVERIFY(spy.isValid());

        const ActivityEvent e = ActivityEvent::forPollCreated(QStringLiteral("folder-A"),
                                                              QStringLiteral("conv-A"),
                                                              QStringLiteral("poll-1"),
                                                              QStringLiteral("agent"),
                                                              QStringLiteral("Coord"),
                                                              QStringLiteral("agent-coord-id"),
                                                              QStringLiteral("Which language?"),
                                                              QStringLiteral("single"));
        QVERIFY(e.isValid());

        m_audit->record(e);

        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.at(0).at(0).toString(), QStringLiteral("folder-A"));
        QCOMPARE(spy.at(0).at(1).toString(), QStringLiteral("conv-A"));

        const QVariantList rows = m_audit->recentActivityForProject(QStringLiteral("folder-A"), 10);
        QCOMPARE(rows.size(), 1);
        const QVariantMap row = rows.first().toMap();
        QCOMPARE(row.value(QStringLiteral("id")).toString(), e.id);
        QCOMPARE(row.value(QStringLiteral("eventType")).toString(), QStringLiteral("poll_created"));
        QCOMPARE(row.value(QStringLiteral("actorKind")).toString(), QStringLiteral("agent"));
        QCOMPARE(row.value(QStringLiteral("actorAlias")).toString(), QStringLiteral("Coord"));
        QCOMPARE(row.value(QStringLiteral("toolName")).toString(), QStringLiteral("start_poll"));
    }

    void test_recentActivityForProject_filtersOrdersClamps() {
        for (int i = 1; i <= 3; ++i) {
            ActivityEvent e = ActivityEvent::forPollCreated(QStringLiteral("folder-A"),
                                                            QStringLiteral("conv-%1").arg(i),
                                                            QStringLiteral("poll-%1").arg(i),
                                                            QStringLiteral("agent"),
                                                            QStringLiteral("Coord"),
                                                            QStringLiteral("agent-coord"),
                                                            QStringLiteral("Question %1").arg(i),
                                                            QStringLiteral("single"));
            e.createdAt = QDateTime::currentDateTimeUtc().addMSecs(i * 10);
            m_audit->record(e);
        }
        m_audit->record(ActivityEvent::forPollCreated(QStringLiteral("folder-B"),
                                                      QStringLiteral("conv-B"),
                                                      QStringLiteral("poll-B"),
                                                      QStringLiteral("agent"),
                                                      QStringLiteral("Coord"),
                                                      QStringLiteral("agent-coord"),
                                                      QStringLiteral("Other question"),
                                                      QStringLiteral("single")));

        const QVariantList aRows =
            m_audit->recentActivityForProject(QStringLiteral("folder-A"), 100);
        QCOMPARE(aRows.size(), 3);

        QCOMPARE(aRows.at(0)
                     .toMap()
                     .value(QStringLiteral("eventDetail"))
                     .toMap()
                     .value(QStringLiteral("question"))
                     .toString(),
                 QStringLiteral("Question 3"));
        QCOMPARE(aRows.at(2)
                     .toMap()
                     .value(QStringLiteral("eventDetail"))
                     .toMap()
                     .value(QStringLiteral("question"))
                     .toString(),
                 QStringLiteral("Question 1"));

        const QVariantList clampedZero =
            m_audit->recentActivityForProject(QStringLiteral("folder-A"), 0);
        QCOMPARE(clampedZero.size(), 1);
        const QVariantList clampedHigh =
            m_audit->recentActivityForProject(QStringLiteral("folder-A"), 99999);
        QCOMPARE(clampedHigh.size(), 3);

        const QVariantList rejectEmpty = m_audit->recentActivityForProject(QString(), 10);
        QCOMPARE(rejectEmpty.size(), 0);
    }

    void test_recentActivityForConversation_filtersAndOrders() {
        for (int i = 1; i <= 2; ++i) {
            ActivityEvent e = ActivityEvent::forMemberAdded(QStringLiteral("folder-A"),
                                                            QStringLiteral("conv-X"),
                                                            QStringLiteral("Alice%1").arg(i),
                                                            QStringLiteral("agent-id"),
                                                            QStringLiteral("user"),
                                                            QStringLiteral("user"),
                                                            QString());
            e.createdAt = QDateTime::currentDateTimeUtc().addMSecs(i * 10);
            m_audit->record(e);
        }
        m_audit->record(ActivityEvent::forMemberAdded(QStringLiteral("folder-A"),
                                                      QStringLiteral("conv-Y"),
                                                      QStringLiteral("Bob"),
                                                      QStringLiteral("agent-id"),
                                                      QStringLiteral("user"),
                                                      QStringLiteral("user"),
                                                      QString()));

        const QVariantList xRows =
            m_audit->recentActivityForConversation(QStringLiteral("conv-X"), 10);
        QCOMPARE(xRows.size(), 2);
        QCOMPARE(xRows.at(0)
                     .toMap()
                     .value(QStringLiteral("eventDetail"))
                     .toMap()
                     .value(QStringLiteral("added_alias"))
                     .toString(),
                 QStringLiteral("Alice2"));
    }

    void test_recentActivityByTurn_ascAndFilters() {
        const QString turn = QStringLiteral("turn-T1");
        for (int i = 1; i <= 3; ++i) {
            ActivityEvent e = ActivityEvent::forToolInvoked(QStringLiteral("folder-A"),
                                                            QStringLiteral("conv-A"),
                                                            turn,
                                                            QStringLiteral("Coord"),
                                                            QStringLiteral("agent-coord"),
                                                            QString(),
                                                            QStringLiteral("read_file"),
                                                            QStringLiteral("path=foo%1").arg(i),
                                                            QStringLiteral("success"));
            e.createdAt = QDateTime::currentDateTimeUtc().addMSecs(i * 10);
            m_audit->record(e);
        }
        m_audit->record(ActivityEvent::forToolInvoked(QStringLiteral("folder-A"),
                                                      QStringLiteral("conv-A"),
                                                      QStringLiteral("turn-OTHER"),
                                                      QStringLiteral("Coord"),
                                                      QStringLiteral("agent-coord"),
                                                      QString(),
                                                      QStringLiteral("write_file"),
                                                      QStringLiteral("path=other"),
                                                      QStringLiteral("success")));

        const QVariantList rows = m_audit->recentActivityByTurn(turn);
        QCOMPARE(rows.size(), 3);
        QCOMPARE(rows.at(0)
                     .toMap()
                     .value(QStringLiteral("eventDetail"))
                     .toMap()
                     .value(QStringLiteral("args_summary"))
                     .toString(),
                 QStringLiteral("path=foo1"));
        QCOMPARE(rows.at(2)
                     .toMap()
                     .value(QStringLiteral("eventDetail"))
                     .toMap()
                     .value(QStringLiteral("args_summary"))
                     .toString(),
                 QStringLiteral("path=foo3"));
    }

    void test_factories_setKindAndType_perSpec() {
        struct Case {
            QString eventType;
            QString actorKind;
            bool hasToolName;
        };

        const auto pollCreated = ActivityEvent::forPollCreated(QString(),
                                                               QStringLiteral("c"),
                                                               QStringLiteral("p"),
                                                               QStringLiteral("agent"),
                                                               QStringLiteral("Coord"),
                                                               QString(),
                                                               QStringLiteral("q?"),
                                                               QStringLiteral("single"));
        QVERIFY(pollCreated.isValid());
        QCOMPARE(pollCreated.eventType, QStringLiteral("poll_created"));
        QCOMPARE(pollCreated.actorKind, QStringLiteral("agent"));
        QCOMPARE(pollCreated.toolName, QStringLiteral("start_poll"));

        const auto imgAgent = ActivityEvent::forImageGenerated(QString(),
                                                               QStringLiteral("c"),
                                                               QStringLiteral("job-1"),
                                                               QStringLiteral("Designer"),
                                                               QStringLiteral("agent-id"),
                                                               QStringLiteral("a cat"),
                                                               QStringLiteral("/tmp/cat.png"));
        QVERIFY(imgAgent.isValid());
        QCOMPARE(imgAgent.eventType, QStringLiteral("image_generated"));
        QCOMPARE(imgAgent.actorKind, QStringLiteral("agent"));
        QCOMPARE(imgAgent.toolName, QStringLiteral("generate_image"));

        const auto imgUser = ActivityEvent::forImageGenerated(QString(),
                                                              QStringLiteral("c"),
                                                              QStringLiteral("job-2"),
                                                              QString(),
                                                              QString(),
                                                              QStringLiteral("a dog"),
                                                              QStringLiteral("/tmp/dog.png"));
        QCOMPARE(imgUser.actorKind, QStringLiteral("user"));
        QCOMPARE(imgUser.actorAlias, QStringLiteral("user"));

        const auto turn = ActivityEvent::forAgentTurn(QString(),
                                                      QStringLiteral("c"),
                                                      QStringLiteral("t"),
                                                      QStringLiteral("Coord"),
                                                      QStringLiteral("aid"),
                                                      QString(),
                                                      QStringLiteral("ollama"),
                                                      QStringLiteral("qwen3:9b"),
                                                      QStringLiteral("stop"),
                                                      100,
                                                      1500);
        QCOMPARE(turn.eventType, QStringLiteral("agent_turn"));
        QCOMPARE(turn.actorKind, QStringLiteral("agent"));
        QVERIFY(turn.toolName.isEmpty());

        const auto denied =
            ActivityEvent::forPermissionDenied(QStringLiteral("f"),
                                               QString(),
                                               QStringLiteral("agent"),
                                               QStringLiteral("Restricted"),
                                               QStringLiteral("aid"),
                                               QString(),
                                               QStringLiteral("tool:shell"),
                                               QStringLiteral("Capability not granted"));
        QCOMPARE(denied.eventType, QStringLiteral("permission_denied"));
        QCOMPARE(denied.actorKind, QStringLiteral("agent"));
        QVERIFY(denied.toolName.isEmpty());

        const auto mountReg =
            ActivityEvent::forWorkspaceMountRegistered(QStringLiteral("folder-1"),
                                                       QStringLiteral("mount-abc"),
                                                       QStringLiteral("client-uuid-7"),
                                                       QStringLiteral("My Project"));
        QCOMPARE(mountReg.eventType, QStringLiteral("workspace.mount.registered"));
        QCOMPARE(mountReg.actorKind, QStringLiteral("client"));
        QCOMPARE(mountReg.actorClientId, QStringLiteral("client-uuid-7"));
        QCOMPARE(mountReg.actorAlias, QStringLiteral("My Project"));
        QVERIFY(mountReg.isValid());

        const auto mountUnreg =
            ActivityEvent::forWorkspaceMountUnregistered(QStringLiteral("folder-1"),
                                                         QStringLiteral("mount-abc"),
                                                         QStringLiteral("client-uuid-7"));
        QCOMPARE(mountUnreg.eventType, QStringLiteral("workspace.mount.unregistered"));
        QCOMPARE(mountUnreg.actorKind, QStringLiteral("client"));
        QVERIFY(mountUnreg.isValid());

        const auto mountReplaced =
            ActivityEvent::forWorkspaceMountReplaced(QStringLiteral("folder-1"),
                                                     QStringLiteral("mount-old"),
                                                     QStringLiteral("client-old"),
                                                     QStringLiteral("mount-new"),
                                                     QStringLiteral("client-new"));
        QCOMPARE(mountReplaced.eventType, QStringLiteral("workspace.mount.replaced"));
        QCOMPARE(mountReplaced.actorKind, QStringLiteral("client"));
        QCOMPARE(mountReplaced.actorClientId, QStringLiteral("client-new"));
        QVERIFY(mountReplaced.isValid());

        const auto mountTree =
            ActivityEvent::forWorkspaceMountTreeUpdated(QStringLiteral("folder-1"),
                                                        QStringLiteral("mount-abc"),
                                                        QStringLiteral("client-uuid-7"));
        QCOMPARE(mountTree.eventType, QStringLiteral("workspace.mount.tree_updated"));
        QCOMPARE(mountTree.actorKind, QStringLiteral("client"));
        QVERIFY(mountTree.isValid());

        const auto mountTier =
            ActivityEvent::forWorkspaceMountTierChanged(QStringLiteral("folder-1"),
                                                        QStringLiteral("mount-abc"),
                                                        QStringLiteral("client-uuid-7"),
                                                        QStringLiteral("ask"),
                                                        QStringLiteral("smart"));
        QCOMPARE(mountTier.eventType, QStringLiteral("workspace.mount.tier_changed"));
        QCOMPARE(mountTier.actorKind, QStringLiteral("client"));
        QVERIFY(mountTier.isValid());

        const auto mountStale =
            ActivityEvent::forWorkspaceMountStale(QStringLiteral("folder-1"),
                                                  QStringLiteral("mount-abc"),
                                                  QStringLiteral("client-uuid-7"),
                                                  1700000000000LL);
        QCOMPARE(mountStale.eventType, QStringLiteral("workspace.mount.stale"));
        QCOMPARE(mountStale.actorKind, QStringLiteral("system"));
        QVERIFY(mountStale.isValid());
    }

    void test_record_workspaceMount_persistsWithClientActorKind() {
        m_audit->record(ActivityEvent::forWorkspaceMountRegistered(QStringLiteral("folder-MNT"),
                                                                   QStringLiteral("mount-1"),
                                                                   QStringLiteral("client-A"),
                                                                   QStringLiteral("Workspace A")));
        m_audit->record(ActivityEvent::forWorkspaceMountUnregistered(
            QStringLiteral("folder-MNT"), QStringLiteral("mount-1"), QStringLiteral("client-A")));
        m_audit->record(ActivityEvent::forWorkspaceMountReplaced(QStringLiteral("folder-MNT"),
                                                                 QStringLiteral("mount-1"),
                                                                 QStringLiteral("client-A"),
                                                                 QStringLiteral("mount-2"),
                                                                 QStringLiteral("client-B")));
        m_audit->record(ActivityEvent::forWorkspaceMountTreeUpdated(
            QStringLiteral("folder-MNT"), QStringLiteral("mount-2"), QStringLiteral("client-B")));
        m_audit->record(ActivityEvent::forWorkspaceMountTierChanged(QStringLiteral("folder-MNT"),
                                                                    QStringLiteral("mount-2"),
                                                                    QStringLiteral("client-B"),
                                                                    QStringLiteral("ask"),
                                                                    QStringLiteral("bypass")));

        const auto rows = m_audit->recentActivityForProject(QStringLiteral("folder-MNT"), 100);
        QCOMPARE(rows.size(), 5);
        for (const auto& v : rows) {
            const auto map = v.toMap();
            QCOMPARE(map.value(QStringLiteral("actorKind")).toString(), QStringLiteral("client"));
        }
    }

    void test_activityLogSchema_includesClientInCheck_afterMigrations() {
        QSqlQuery q(QSqlDatabase::database(QStringLiteral("verzeta_main")));
        QVERIFY(
            q.exec(QStringLiteral("SELECT sql FROM sqlite_master WHERE name = 'activity_log'")));
        QVERIFY(q.next());
        const QString actualSql = q.value(0).toString();
        QVERIFY2(actualSql.contains(QStringLiteral("'client'")),
                 qPrintable(QStringLiteral("activity_log CHECK constraint is missing 'client' — "
                                           "v18 migration may not have run. sqlite_master.sql: ") +
                            actualSql));
        QVERIFY2(actualSql.contains(QStringLiteral("'user'")),
                 "activity_log CHECK constraint is missing 'user'");
        QVERIFY2(actualSql.contains(QStringLiteral("'agent'")),
                 "activity_log CHECK constraint is missing 'agent'");
        QVERIFY2(actualSql.contains(QStringLiteral("'system'")),
                 "activity_log CHECK constraint is missing 'system'");
    }

    void test_memberFactories_emptyAlias_nonNullActorAlias() {
        const auto userAdd = ActivityEvent::forMemberAdded(QStringLiteral("folder-MA"),
                                                           QString(),
                                                           QStringLiteral("Alice"),
                                                           QStringLiteral("agent-alice"),
                                                           QStringLiteral("user"),
                                                           QString(),
                                                           QString());
        QVERIFY(!userAdd.actorAlias.isNull());
        QCOMPARE(userAdd.actorAlias, QStringLiteral("user"));
        QVERIFY(userAdd.isValid());

        const auto sysRemove = ActivityEvent::forMemberRemoved(QStringLiteral("folder-MR"),
                                                               QString(),
                                                               QStringLiteral("Bob"),
                                                               QStringLiteral("agent-bob"),
                                                               QStringLiteral("system"),
                                                               QString(),
                                                               QString());
        QVERIFY(!sysRemove.actorAlias.isNull());
        QVERIFY(sysRemove.actorAlias.isEmpty());
        QVERIFY(sysRemove.isValid());

        m_audit->record(userAdd);
        const QVariantList rows =
            m_audit->recentActivityForProject(QStringLiteral("folder-MA"), 10);
        QCOMPARE(rows.size(), 1);
        QCOMPARE(rows.first().toMap().value(QStringLiteral("actorAlias")).toString(),
                 QStringLiteral("user"));
    }

    void test_eventDetail_jsonRoundTrip() {
        ActivityEvent e = ActivityEvent::forAgentTurn(QStringLiteral("folder-A"),
                                                      QStringLiteral("conv-A"),
                                                      QStringLiteral("turn-T2"),
                                                      QStringLiteral("Coord"),
                                                      QStringLiteral("agent-coord"),
                                                      QStringLiteral("local"),
                                                      QStringLiteral("openrouter"),
                                                      QStringLiteral("openrouter/free"),
                                                      QStringLiteral("stop"),
                                                      512,
                                                      7777);
        m_audit->record(e);

        const QVariantList rows =
            m_audit->recentActivityForConversation(QStringLiteral("conv-A"), 10);
        QCOMPARE(rows.size(), 1);
        const QVariantMap detail =
            rows.first().toMap().value(QStringLiteral("eventDetail")).toMap();
        QCOMPARE(detail.value(QStringLiteral("provider_id")).toString(),
                 QStringLiteral("openrouter"));
        QCOMPARE(detail.value(QStringLiteral("model_name")).toString(),
                 QStringLiteral("openrouter/free"));
        QCOMPARE(detail.value(QStringLiteral("finish_reason")).toString(), QStringLiteral("stop"));
        QCOMPARE(detail.value(QStringLiteral("total_tokens")).toInt(), 512);
        QCOMPARE(detail.value(QStringLiteral("elapsed_ms")).toLongLong(),
                 static_cast<qint64>(7777));
    }
};

QTEST_MAIN(TestAuditService)
#include "test-audit-service.moc"
