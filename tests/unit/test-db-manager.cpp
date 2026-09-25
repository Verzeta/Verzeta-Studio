// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/db-manager.h"

#include <QTemporaryDir>
#include <QtTest>

#include <QDateTime>
#include <QFile>
#include <QSet>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlRecord>
#include <QString>

class TestDbManager : public QObject {
    Q_OBJECT

  private slots:
    void init() {
        QVERIFY(m_tempDir.isValid());
        m_dbPath = m_tempDir.path() +
                   QStringLiteral("/test_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    }

    void cleanup() {
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QFile::remove(m_dbPath);
    }


    void testSchemaCreation() {
        QVERIFY(DbManager::instance().open(m_dbPath));
        QVERIFY(DbManager::instance().runMigrations());
        QVERIFY(DbManager::instance().isOpen());

        QSqlDatabase db = DbManager::instance().db();

        const QStringList expectedTables = {
            QStringLiteral("conversations"),
            QStringLiteral("folders"),
            QStringLiteral("messages"),
            QStringLiteral("attachments"),
            QStringLiteral("tool_calls"),
            QStringLiteral("embeddings"),
            QStringLiteral("documents"),
            QStringLiteral("settings"),
            QStringLiteral("messages_fts"),
            QStringLiteral("canvas_artifacts"),
            QStringLiteral("heartbeat_configs"),
            QStringLiteral("heartbeat_reports"),
        };

        const QStringList actualTables = db.tables();
        for (const QString& table : expectedTables) {
            QVERIFY2(actualTables.contains(table),
                     qPrintable(QStringLiteral("Missing table: ") + table));
        }
    }


    void testSchemaVersioning() {
        QVERIFY(DbManager::instance().open(m_dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        QSqlQuery q(DbManager::instance().db());
        QVERIFY(q.exec(
            QStringLiteral("SELECT value FROM settings WHERE key='schema_version' LIMIT 1")));
        QVERIFY(q.next());
        QCOMPARE(q.value(0).toInt(), 24);
    }

    void testSchemaV23AgentMemories() {
        QVERIFY(DbManager::instance().open(m_dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        auto hasColumn = [](const QString& table, const QString& column) -> bool {
            QSqlQuery q(DbManager::instance().db());
            if (!q.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table))) {
                return false;
            }
            while (q.next()) {
                if (q.value(1).toString() == column)
                    return true;
            }
            return false;
        };
        for (const QString& col : {QStringLiteral("id"),
                                   QStringLiteral("owner_scope"),
                                   QStringLiteral("kind"),
                                   QStringLiteral("text"),
                                   QStringLiteral("embedding"),
                                   QStringLiteral("model_used"),
                                   QStringLiteral("source_conv_id"),
                                   QStringLiteral("source_msg_id"),
                                   QStringLiteral("confidence"),
                                   QStringLiteral("use_count"),
                                   QStringLiteral("last_used_at_ms"),
                                   QStringLiteral("superseded_by"),
                                   QStringLiteral("created_at_ms")}) {
            QVERIFY2(hasColumn(QStringLiteral("agent_memories"), col),
                     qPrintable(QStringLiteral("agent_memories.%1 missing").arg(col)));
        }

        QSqlQuery m(DbManager::instance().db());
        QVERIFY(m.exec(
            QStringLiteral("SELECT name FROM sqlite_master WHERE name='agent_memories_fts'")));
        QVERIFY(m.next());
        QSqlQuery idx(DbManager::instance().db());
        QVERIFY(idx.exec(QStringLiteral("SELECT name FROM sqlite_master WHERE type='index' "
                                        "AND name='idx_agent_memories_scope'")));
        QVERIFY(idx.next());

        QSqlQuery bad(DbManager::instance().db());
        bad.prepare(
            QStringLiteral("INSERT INTO agent_memories(id, owner_scope, kind, text, created_at_ms) "
                           "VALUES('m1','agent:A','bogus','x',0)"));
        QVERIFY2(!bad.exec(), "agent_memories.kind CHECK should reject 'bogus'");
    }

    void testSchemaV24AcnEntries() {
        QVERIFY(DbManager::instance().open(m_dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        auto hasColumn = [](const QString& table, const QString& column) -> bool {
            QSqlQuery q(DbManager::instance().db());
            if (!q.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table))) {
                return false;
            }
            while (q.next()) {
                if (q.value(1).toString() == column)
                    return true;
            }
            return false;
        };
        for (const QString& col : {QStringLiteral("id"),
                                   QStringLiteral("owner_scope"),
                                   QStringLiteral("entry_kind"),
                                   QStringLiteral("text"),
                                   QStringLiteral("embedding"),
                                   QStringLiteral("model_used"),
                                   QStringLiteral("source_conv_id"),
                                   QStringLiteral("covered_range"),
                                   QStringLiteral("created_at_ms")}) {
            QVERIFY2(hasColumn(QStringLiteral("acn_entries"), col),
                     qPrintable(QStringLiteral("acn_entries.%1 missing").arg(col)));
        }
        QVERIFY2(hasColumn(QStringLiteral("folders"), QStringLiteral("acn_enabled")),
                 "folders.acn_enabled missing");

        QSqlQuery m(DbManager::instance().db());
        QVERIFY(
            m.exec(QStringLiteral("SELECT name FROM sqlite_master WHERE name='acn_entries_fts'")));
        QVERIFY(m.next());
        QSqlQuery idx(DbManager::instance().db());
        QVERIFY(idx.exec(QStringLiteral("SELECT name FROM sqlite_master WHERE type='index' "
                                        "AND name='idx_acn_entries_scope'")));
        QVERIFY(idx.next());

        QSqlQuery bad(DbManager::instance().db());
        bad.prepare(QStringLiteral(
            "INSERT INTO acn_entries(id, owner_scope, entry_kind, text, created_at_ms) "
            "VALUES('a1','project:F','bogus','x',0)"));
        QVERIFY2(!bad.exec(), "acn_entries.entry_kind CHECK should reject 'bogus'");

        QSqlQuery f(DbManager::instance().db());
        QVERIFY(f.exec(QStringLiteral("INSERT INTO folders(id, name, created_at, folder_type) "
                                      "VALUES('proj1','P',0,'project')")));
        QSqlQuery sel(DbManager::instance().db());
        QVERIFY(sel.exec(QStringLiteral("SELECT acn_enabled FROM folders WHERE id='proj1'")));
        QVERIFY(sel.next());
        QCOMPARE(sel.value(0).toInt(), 1);
    }

    void testSchemaV14Columns() {
        QVERIFY(DbManager::instance().open(m_dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        auto hasColumn = [](const QString& table, const QString& column) -> bool {
            QSqlQuery q(DbManager::instance().db());
            if (!q.exec(QStringLiteral("PRAGMA table_info(%1)").arg(table))) {
                return false;
            }
            while (q.next()) {
                if (q.value(1).toString() == column)
                    return true;
            }
            return false;
        };

        for (const QString& table :
             {QStringLiteral("project_members"), QStringLiteral("conversation_members")}) {
            QVERIFY2(hasColumn(table, QStringLiteral("model_provider")),
                     qPrintable(table + QStringLiteral(".model_provider missing")));
            QVERIFY2(hasColumn(table, QStringLiteral("model_name")),
                     qPrintable(table + QStringLiteral(".model_name missing")));
            QVERIFY2(hasColumn(table, QStringLiteral("allowed_tools")),
                     qPrintable(table + QStringLiteral(".allowed_tools missing")));
        }
        QVERIFY2(hasColumn(QStringLiteral("conversations"), QStringLiteral("member_alias")),
                 "conversations.member_alias missing");
    }

    void testSchemaV15Columns() {
        QVERIFY(DbManager::instance().open(m_dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        QSqlQuery info(DbManager::instance().db());
        QVERIFY(info.exec(QStringLiteral("PRAGMA table_info(activity_log)")));
        QSet<QString> presentColumns;
        while (info.next()) {
            presentColumns.insert(info.value(1).toString());
        }
        const QStringList expected = {
            QStringLiteral("id"),
            QStringLiteral("created_at"),
            QStringLiteral("project_folder_id"),
            QStringLiteral("conversation_id"),
            QStringLiteral("turn_id"),
            QStringLiteral("actor_kind"),
            QStringLiteral("actor_alias"),
            QStringLiteral("actor_agent_id"),
            QStringLiteral("actor_client_id"),
            QStringLiteral("event_type"),
            QStringLiteral("tool_name"),
            QStringLiteral("event_summary"),
            QStringLiteral("event_detail"),
        };
        for (const QString& col : expected) {
            QVERIFY2(presentColumns.contains(col),
                     qPrintable(QStringLiteral("activity_log.%1 missing").arg(col)));
        }

        QSqlQuery idxList(DbManager::instance().db());
        QVERIFY(idxList.exec(QStringLiteral("PRAGMA index_list(activity_log)")));
        QSet<QString> presentIndexes;
        while (idxList.next()) {
            presentIndexes.insert(idxList.value(1).toString());
        }
        QVERIFY2(presentIndexes.contains(QStringLiteral("idx_activity_project")),
                 "idx_activity_project missing");
        QVERIFY2(presentIndexes.contains(QStringLiteral("idx_activity_conv")),
                 "idx_activity_conv missing");
        QVERIFY2(presentIndexes.contains(QStringLiteral("idx_activity_turn")),
                 "idx_activity_turn missing");
    }

    void testSchemaV18ActorKindWidened() {
        QVERIFY(DbManager::instance().open(m_dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        QSqlQuery ver(DbManager::instance().db());
        QVERIFY(
            ver.exec(QStringLiteral("SELECT value FROM settings WHERE key = 'schema_version'")));
        QVERIFY(ver.next());
        QCOMPARE(ver.value(0).toInt(), 24);

        QSqlQuery info(DbManager::instance().db());
        QVERIFY(info.exec(QStringLiteral("PRAGMA table_info(activity_log)")));
        QSet<QString> presentColumns;
        while (info.next())
            presentColumns.insert(info.value(1).toString());
        const QStringList expected = {
            QStringLiteral("id"),
            QStringLiteral("created_at"),
            QStringLiteral("project_folder_id"),
            QStringLiteral("conversation_id"),
            QStringLiteral("turn_id"),
            QStringLiteral("actor_kind"),
            QStringLiteral("actor_alias"),
            QStringLiteral("actor_agent_id"),
            QStringLiteral("actor_client_id"),
            QStringLiteral("event_type"),
            QStringLiteral("tool_name"),
            QStringLiteral("event_summary"),
            QStringLiteral("event_detail"),
        };
        for (const QString& col : expected) {
            QVERIFY2(
                presentColumns.contains(col),
                qPrintable(QStringLiteral("activity_log.%1 missing after v18 migration").arg(col)));
        }

        QSqlQuery checkSql(DbManager::instance().db());
        QVERIFY(checkSql.exec(
            QStringLiteral("SELECT sql FROM sqlite_master WHERE name = 'activity_log'")));
        QVERIFY(checkSql.next());
        const QString actualSql = checkSql.value(0).toString();
        QVERIFY2(actualSql.contains(QStringLiteral("'client'")),
                 qPrintable(QStringLiteral("v18 CHECK widening NOT applied. sqlite_master.sql: ") +
                            actualSql));
        QVERIFY2(actualSql.contains(QStringLiteral("'user'")), "post-v18 CHECK is missing 'user'");
        QVERIFY2(actualSql.contains(QStringLiteral("'agent'")),
                 "post-v18 CHECK is missing 'agent'");
        QVERIFY2(actualSql.contains(QStringLiteral("'system'")),
                 "post-v18 CHECK is missing 'system'");

        QSqlQuery idxList(DbManager::instance().db());
        QVERIFY(idxList.exec(QStringLiteral("PRAGMA index_list(activity_log)")));
        QSet<QString> presentIndexes;
        while (idxList.next())
            presentIndexes.insert(idxList.value(1).toString());
        QVERIFY2(presentIndexes.contains(QStringLiteral("idx_activity_project")),
                 "idx_activity_project not recreated by v18");
        QVERIFY2(presentIndexes.contains(QStringLiteral("idx_activity_conv")),
                 "idx_activity_conv not recreated by v18");
        QVERIFY2(presentIndexes.contains(QStringLiteral("idx_activity_turn")),
                 "idx_activity_turn not recreated by v18");
    }

    void testSchemaV18AcceptsClientActorKind() {
        QVERIFY(DbManager::instance().open(m_dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        QSqlQuery ins(DbManager::instance().db());
        ins.prepare(QStringLiteral("INSERT INTO activity_log "
                                   "(id, created_at, project_folder_id, conversation_id, turn_id,"
                                   " actor_kind, actor_alias, actor_agent_id, actor_client_id,"
                                   " event_type, tool_name, event_summary, event_detail)"
                                   " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
        ins.addBindValue(QStringLiteral("test-id-v18-client"));
        ins.addBindValue(QStringLiteral("2026-06-05T18:14:00.000Z"));
        ins.addBindValue(QStringLiteral("test-folder"));
        ins.addBindValue(QVariant());
        ins.addBindValue(QVariant());
        ins.addBindValue(QStringLiteral("client"));
        ins.addBindValue(QStringLiteral("Test Workspace"));
        ins.addBindValue(QVariant());
        ins.addBindValue(QStringLiteral("client-uuid-test"));
        ins.addBindValue(QStringLiteral("workspace.mount.registered"));
        ins.addBindValue(QVariant());
        ins.addBindValue(QStringLiteral("Workspace mount registered: Test Workspace"));
        ins.addBindValue(QStringLiteral("{}"));
        QVERIFY2(ins.exec(),
                 qPrintable(QStringLiteral("v18 INSERT with actor_kind='client' rejected: ") +
                            ins.lastError().text()));

        QSqlQuery read(DbManager::instance().db());
        QVERIFY(read.exec(QStringLiteral("SELECT actor_kind FROM activity_log "
                                         "WHERE id = 'test-id-v18-client'")));
        QVERIFY(read.next());
        QCOMPARE(read.value(0).toString(), QStringLiteral("client"));
    }

    void testSchemaV18RejectsUnknownActorKind() {
        QVERIFY(DbManager::instance().open(m_dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        QSqlQuery ins(DbManager::instance().db());
        ins.prepare(QStringLiteral("INSERT INTO activity_log "
                                   "(id, created_at, project_folder_id, conversation_id, turn_id,"
                                   " actor_kind, actor_alias, actor_agent_id, actor_client_id,"
                                   " event_type, tool_name, event_summary, event_detail)"
                                   " VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)"));
        ins.addBindValue(QStringLiteral("test-id-v18-bogus"));
        ins.addBindValue(QStringLiteral("2026-06-05T18:14:00.000Z"));
        ins.addBindValue(QStringLiteral("test-folder"));
        ins.addBindValue(QVariant());
        ins.addBindValue(QVariant());
        ins.addBindValue(QStringLiteral("pirate"));
        ins.addBindValue(QStringLiteral("Bogus"));
        ins.addBindValue(QVariant());
        ins.addBindValue(QVariant());
        ins.addBindValue(QStringLiteral("agent_turn"));
        ins.addBindValue(QVariant());
        ins.addBindValue(QStringLiteral("test"));
        ins.addBindValue(QVariant());
        QVERIFY2(!ins.exec(), "v18 CHECK accepted 'pirate' — enum widening went too far");
    }


    void testIdempotentMigration() {
        QVERIFY(DbManager::instance().open(m_dbPath));
        QVERIFY(DbManager::instance().runMigrations());
        QVERIFY(DbManager::instance().runMigrations());
        QVERIFY(DbManager::instance().isOpen());
    }


    void testConversationInsert() {
        QVERIFY(DbManager::instance().open(m_dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        QSqlQuery q(DbManager::instance().db());
        q.prepare(QStringLiteral("INSERT INTO conversations(id, title, created_at, updated_at) "
                                 "VALUES(?, ?, ?, ?)"));
        q.addBindValue(QStringLiteral("conv-001"));
        q.addBindValue(QStringLiteral("Test Conversation"));
        q.addBindValue(1000LL);
        q.addBindValue(1000LL);
        QVERIFY(q.exec());

        QSqlQuery readQ(DbManager::instance().db());
        readQ.prepare(QStringLiteral("SELECT * FROM conversations WHERE id = ?"));
        readQ.addBindValue(QStringLiteral("conv-001"));
        QVERIFY(readQ.exec());
        QVERIFY(readQ.next());
        QCOMPARE(readQ.record().value(QStringLiteral("title")).toString(),
                 QStringLiteral("Test Conversation"));
    }


    void testMessageInsert() {
        QVERIFY(DbManager::instance().open(m_dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        QSqlQuery cq(DbManager::instance().db());
        cq.prepare(QStringLiteral("INSERT INTO conversations(id, title, created_at, updated_at) "
                                  "VALUES(?, ?, ?, ?)"));
        cq.addBindValue(QStringLiteral("conv-002"));
        cq.addBindValue(QStringLiteral("Conv for Message Test"));
        cq.addBindValue(1000LL);
        cq.addBindValue(1000LL);
        QVERIFY(cq.exec());

        QSqlQuery q(DbManager::instance().db());
        q.prepare(
            QStringLiteral("INSERT INTO messages(id, conversation_id, role, content, created_at) "
                           "VALUES(?, ?, ?, ?, ?)"));
        q.addBindValue(QStringLiteral("msg-001"));
        q.addBindValue(QStringLiteral("conv-002"));
        q.addBindValue(QStringLiteral("user"));
        q.addBindValue(QStringLiteral("Hello, world!"));
        q.addBindValue(2000LL);
        QVERIFY(q.exec());

        QSqlQuery readQ(DbManager::instance().db());
        readQ.prepare(QStringLiteral("SELECT * FROM messages WHERE id = ?"));
        readQ.addBindValue(QStringLiteral("msg-001"));
        QVERIFY(readQ.exec());
        QVERIFY(readQ.next());

        const QSqlRecord rec = readQ.record();
        QCOMPARE(rec.value(QStringLiteral("role")).toString(), QStringLiteral("user"));
        QCOMPARE(rec.value(QStringLiteral("content")).toString(), QStringLiteral("Hello, world!"));
        QCOMPARE(rec.value(QStringLiteral("conversation_id")).toString(),
                 QStringLiteral("conv-002"));
    }


    void testForeignKeyCascade() {
        QVERIFY(DbManager::instance().open(m_dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        const QString convId = QStringLiteral("conv-cascade");
        const QString msgId = QStringLiteral("msg-cascade");

        {
            QSqlQuery q(DbManager::instance().db());
            q.prepare(QStringLiteral("INSERT INTO conversations(id, title, created_at, updated_at) "
                                     "VALUES(?, ?, ?, ?)"));
            q.addBindValue(convId);
            q.addBindValue(QStringLiteral("Cascade Test"));
            q.addBindValue(1000LL);
            q.addBindValue(1000LL);
            QVERIFY(q.exec());
        }

        {
            QSqlQuery q(DbManager::instance().db());
            q.prepare(QStringLiteral(
                "INSERT INTO messages(id, conversation_id, role, content, created_at) "
                "VALUES(?, ?, ?, ?, ?)"));
            q.addBindValue(msgId);
            q.addBindValue(convId);
            q.addBindValue(QStringLiteral("assistant"));
            q.addBindValue(QStringLiteral("Cascaded message"));
            q.addBindValue(2000LL);
            QVERIFY(q.exec());
        }

        {
            QSqlQuery q(DbManager::instance().db());
            q.prepare(QStringLiteral("DELETE FROM conversations WHERE id = ?"));
            q.addBindValue(convId);
            QVERIFY(q.exec());
        }

        QSqlQuery check(DbManager::instance().db());
        check.prepare(QStringLiteral("SELECT COUNT(*) FROM messages WHERE id = ?"));
        check.addBindValue(msgId);
        QVERIFY(check.exec());
        QVERIFY(check.next());
        QCOMPARE(check.value(0).toInt(), 0);
    }


    void testSqlInjectionPrevention() {
        QVERIFY(DbManager::instance().open(m_dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        const QString injectionTitle = QStringLiteral("'; DROP TABLE conversations; --");

        QSqlQuery q(DbManager::instance().db());
        q.prepare(QStringLiteral("INSERT INTO conversations(id, title, created_at, updated_at) "
                                 "VALUES(?, ?, ?, ?)"));
        q.addBindValue(QStringLiteral("conv-injection"));
        q.addBindValue(injectionTitle);
        q.addBindValue(1000LL);
        q.addBindValue(1000LL);
        QVERIFY(q.exec());

        QVERIFY(DbManager::instance().db().tables().contains(QStringLiteral("conversations")));

        QSqlQuery readQ(DbManager::instance().db());
        readQ.prepare(QStringLiteral("SELECT title FROM conversations WHERE id = ?"));
        readQ.addBindValue(QStringLiteral("conv-injection"));
        QVERIFY(readQ.exec());
        QVERIFY(readQ.next());
        QCOMPARE(readQ.value(0).toString(), injectionTitle);
    }


    void testTransactionRollback() {
        QVERIFY(DbManager::instance().open(m_dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        QVERIFY(DbManager::instance().transaction());

        QSqlQuery q(DbManager::instance().db());
        q.prepare(QStringLiteral("INSERT INTO conversations(id, title, created_at, updated_at) "
                                 "VALUES(?, ?, ?, ?)"));
        q.addBindValue(QStringLiteral("conv-rollback"));
        q.addBindValue(QStringLiteral("Will be rolled back"));
        q.addBindValue(1000LL);
        q.addBindValue(1000LL);
        QVERIFY(q.exec());

        QVERIFY(DbManager::instance().rollback());

        QSqlQuery check(DbManager::instance().db());
        check.prepare(QStringLiteral("SELECT COUNT(*) FROM conversations WHERE id = ?"));
        check.addBindValue(QStringLiteral("conv-rollback"));
        QVERIFY(check.exec());
        QVERIFY(check.next());
        QCOMPARE(check.value(0).toInt(), 0);
    }

  private:
    QTemporaryDir m_tempDir;
    QString m_dbPath;
};

QTEST_MAIN(TestDbManager)
#include "test-db-manager.moc"
