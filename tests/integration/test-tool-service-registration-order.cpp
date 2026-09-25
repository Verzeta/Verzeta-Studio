// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/db-manager.h"
#include "services/conversation-service.h"
#include "services/file-service.h"
#include "services/message-service.h"
#include "services/plan-service.h"
#include "services/task-controller.h"
#include "services/task-runner.h"
#include "services/tool-service.h"
#include "utils/process-sandbox.h"

#include <QTemporaryDir>
#include <QTest>

#include <memory>
#include <QDateTime>
#include <QJsonObject>
#include <QSqlDatabase>
#include <QVariantMap>

class TestToolServiceRegistrationOrder : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<MessageService> m_msgSvc;
    std::unique_ptr<PlanService> m_planSvc;
    std::unique_ptr<TaskRunner> m_taskRunner;
    std::unique_ptr<ProcessSandbox> m_sandbox;
    std::unique_ptr<FileService> m_fileSvc;
    std::unique_ptr<ToolService> m_toolSvc;
    std::unique_ptr<TaskController> m_taskCtrl;

    static const QStringList& taskToolNames() {
        static const QStringList kNames = {
            QStringLiteral("start_task"),
            QStringLiteral("complete_task"),
            QStringLiteral("get_task_status"),
            QStringLiteral("stop_task"),
        };
        return kNames;
    }

  private slots:
    void init() {
        const QString dbPath =
            m_tempDir.path() +
            QStringLiteral("/tsro_%1.db").arg(QDateTime::currentMSecsSinceEpoch());
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_convSvc = std::make_unique<ConversationService>(DbManager::instance());
        m_msgSvc = std::make_unique<MessageService>(DbManager::instance());
        m_planSvc = std::make_unique<PlanService>(DbManager::instance());
        m_taskRunner = std::make_unique<TaskRunner>(*m_planSvc);
        m_sandbox = std::make_unique<ProcessSandbox>();
        m_fileSvc = std::make_unique<FileService>();
        m_toolSvc = std::make_unique<ToolService>();

        m_toolSvc->registerBuiltInTools(*m_sandbox, *m_fileSvc);

        m_taskCtrl =
            std::make_unique<TaskController>(*m_convSvc, *m_msgSvc, *m_planSvc, *m_taskRunner);
        m_taskCtrl->registerTaskToolStubs(*m_toolSvc);
    }

    void cleanup() {
        m_taskCtrl.reset();
        m_toolSvc.reset();
        m_fileSvc.reset();
        m_sandbox.reset();
        m_taskRunner.reset();
        m_planSvc.reset();
        m_msgSvc.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    }

    void test_taskToolStubsRegistered_afterTaskControllerCallsRegister() {
        const QVariantList tools = m_toolSvc->registeredToolsList();

        QSet<QString> presentNames;
        QHash<QString, QString> nameToKind;
        for (const QVariant& v : tools) {
            const QVariantMap m = v.toMap();
            const QString name = m.value(QStringLiteral("name")).toString();
            const QString kind = m.value(QStringLiteral("kind")).toString();
            presentNames.insert(name);
            nameToKind[name] = kind;
        }

        for (const QString& name : taskToolNames()) {
            QVERIFY2(presentNames.contains(name),
                     qPrintable(QStringLiteral("task-tool '%1' missing from registeredToolsList "
                                               "after registerTaskToolStubs")
                                    .arg(name)));
            QCOMPARE(nameToKind.value(name), QStringLiteral("builtin"));
        }
    }

    void test_taskToolStubs_runOnMainThread() {
        for (const QString& name : taskToolNames()) {
            QVERIFY2(m_toolSvc->runsOnMainThread(name),
                     qPrintable(QStringLiteral("task-tool '%1' is not flagged main-thread-only "
                                               "— Chat::ToolDispatcher would dispatch it to a "
                                               "worker thread, where SQLite access would crash")
                                    .arg(name)));
        }
    }

    void test_customTool_collidingWithTaskToolName_rejected() {
        for (const QString& name : taskToolNames()) {
            QVariantMap def;
            def[QStringLiteral("name")] = name;
            def[QStringLiteral("description")] = QStringLiteral("attempted custom override");
            def[QStringLiteral("commandTemplate")] = QString();
            def[QStringLiteral("parameters")] = QVariantList{};

            const bool added = m_toolSvc->addCustomTool(def);
            QVERIFY2(!added,
                     qPrintable(QStringLiteral("addCustomTool('%1') should be rejected because the "
                                               "name collides with a task-tool stub registered as "
                                               "BuiltIn — m_builtInNames invariant violated")
                                    .arg(name)));
        }
    }

    void test_customTool_withNonCollidingName_accepted() {
        const QString uniqueName = QStringLiteral("registration_order_unique_custom_tool");
        QVariantMap def;
        def[QStringLiteral("name")] = uniqueName;
        def[QStringLiteral("description")] = QStringLiteral("non-colliding custom tool");
        def[QStringLiteral("commandTemplate")] = QString();
        def[QStringLiteral("parameters")] = QVariantList{};

        QVERIFY(m_toolSvc->addCustomTool(def));
        QVERIFY(m_toolSvc->hasTool(uniqueName));
    }

    void test_unimplementedStubTools_notRegistered() {
        for (const QString& name : {QStringLiteral("delegate_task"),
                                    QStringLiteral("add_plan_step"),
                                    QStringLiteral("approve_step"),
                                    QStringLiteral("reject_step")}) {
            QVERIFY2(
                !m_toolSvc->hasTool(name),
                qPrintable(QStringLiteral("unimplemented stub tool '%1' must not be registered")
                               .arg(name)));
        }
    }
};

QTEST_MAIN(TestToolServiceRegistrationOrder)
#include "test-tool-service-registration-order.moc"
