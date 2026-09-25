// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/artifacts-model.h"
#include "models/db-manager.h"
#include "services/conversation-service.h"
#include "services/message-service.h"

#include <QTemporaryDir>
#include <QTest>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QUuid>

class TestArtifactsModel : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;
    std::unique_ptr<ConversationService> m_convSvc;
    std::unique_ptr<MessageService> m_msgSvc;
    std::unique_ptr<ArtifactsModel> m_model;
    QString m_convId;

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
        m_msgSvc = std::make_unique<MessageService>(DbManager::instance());
        m_model = std::make_unique<ArtifactsModel>(*m_msgSvc);

        m_convId = m_convSvc->createConversation(QStringLiteral("Chat"));
    }

    void cleanup() {
        m_model.reset();
        m_msgSvc.reset();
        m_convSvc.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    }

    QString writeFileToolMessage(const QString& convId, const QString& path) {
        Message m;
        m.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        m.conversationId = convId;
        m.role = QStringLiteral("tool");
        m.content = QString::fromUtf8(QJsonDocument(QJsonObject{
                                                        {QStringLiteral("path"), path},
                                                        {QStringLiteral("written"), true},
                                                    })
                                          .toJson(QJsonDocument::Compact));
        m.createdAt = QDateTime::currentDateTimeUtc();
        m.metadata = QJsonObject{{QStringLiteral("tool_name"), QStringLiteral("write_file")}};
        return m_msgSvc->addMessage(m);
    }

    QString writePlanArtifactMessage(const QString& convId, const QString& summary) {
        Message m;
        m.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        m.conversationId = convId;
        m.role = QStringLiteral("assistant");
        m.content = QStringLiteral("draft content");
        m.finishReason = QStringLiteral("artifact");
        m.createdAt = QDateTime::currentDateTimeUtc();
        m.memberAlias = QStringLiteral("writer");
        m.metadata = QJsonObject{
            {QStringLiteral("plan_artifact"), true},
            {QStringLiteral("summary"), summary},
            {QStringLiteral("plan_id"), QStringLiteral("plan-1")},
            {QStringLiteral("step_id"), QStringLiteral("step-1")},
            {QStringLiteral("step_title"), QStringLiteral("Write hero")},
            {QStringLiteral("plan_goal"), QStringLiteral("Landing page")},
        };
        return m_msgSvc->addMessage(m);
    }


    void test_initialState_isEmpty() { QTRY_COMPARE(m_model->rowCount(), 0); }

    void test_fileToolMessage_producesRow() {
        m_model->setActiveConversation(m_convId);
        QSignalSpy insertedSpy(m_model.get(), &QAbstractItemModel::rowsInserted);

        writeFileToolMessage(m_convId, QStringLiteral("/tmp/artifacts/hello.txt"));

        QCOMPARE(insertedSpy.count(), 1);
        QTRY_COMPARE(m_model->rowCount(), 1);
        const QModelIndex idx = m_model->index(0, 0);
        QCOMPARE(m_model->data(idx, ArtifactsModel::PathRole).toString(),
                 QStringLiteral("/tmp/artifacts/hello.txt"));
        QCOMPARE(m_model->data(idx, ArtifactsModel::FileNameRole).toString(),
                 QStringLiteral("hello.txt"));
        QCOMPARE(m_model->data(idx, ArtifactsModel::ToolNameRole).toString(),
                 QStringLiteral("write_file"));
    }

    void test_toolMessage_withoutPath_producesNoRow() {
        m_model->setActiveConversation(m_convId);
        Message m;
        m.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        m.conversationId = m_convId;
        m.role = QStringLiteral("tool");
        m.content = QString::fromUtf8(
            QJsonDocument(QJsonObject{{QStringLiteral("stdout"), QStringLiteral("ok")}})
                .toJson(QJsonDocument::Compact));
        m.createdAt = QDateTime::currentDateTimeUtc();
        m.metadata = QJsonObject{{QStringLiteral("tool_name"), QStringLiteral("run_shell")}};
        m_msgSvc->addMessage(m);

        QTRY_COMPARE(m_model->rowCount(), 0);
    }

    void test_planArtifactMessage_producesRow() {
        m_model->setActiveConversation(m_convId);
        writePlanArtifactMessage(m_convId, QStringLiteral("Hero draft"));

        QTRY_COMPARE(m_model->rowCount(), 1);
        const QModelIndex idx = m_model->index(0, 0);
        QCOMPARE(m_model->data(idx, ArtifactsModel::FileNameRole).toString(),
                 QStringLiteral("Hero draft"));
        QCOMPARE(m_model->data(idx, ArtifactsModel::ToolNameRole).toString(),
                 QStringLiteral("submit_result"));
        QCOMPARE(m_model->data(idx, ArtifactsModel::StepTitleRole).toString(),
                 QStringLiteral("Write hero"));
        QCOMPARE(m_model->data(idx, ArtifactsModel::SubmittedByRole).toString(),
                 QStringLiteral("writer"));
    }

    void test_generatedImageMessage_producesRow() {
        m_model->setActiveConversation(m_convId);

        Message m;
        m.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        m.conversationId = m_convId;
        m.role = QStringLiteral("assistant");
        m.content = QStringLiteral("Generated image: hero shot");
        m.finishReason = QStringLiteral("stop");
        m.createdAt = QDateTime::currentDateTimeUtc();
        m.metadata = QJsonObject{
            {QStringLiteral("produced_by"), QStringLiteral("image_service")},
            {QStringLiteral("workspace_path"),
             QStringLiteral("/proj/images/hero-shot-abc12345.png")},
            {QStringLiteral("workspace_rel"), QStringLiteral("images/hero-shot-abc12345.png")},
        };
        m_msgSvc->addMessage(m);

        QTRY_COMPARE(m_model->rowCount(), 1);
        const QModelIndex idx = m_model->index(0, 0);
        QCOMPARE(m_model->data(idx, ArtifactsModel::PathRole).toString(),
                 QStringLiteral("/proj/images/hero-shot-abc12345.png"));
        QCOMPARE(m_model->data(idx, ArtifactsModel::FileNameRole).toString(),
                 QStringLiteral("hero-shot-abc12345.png"));
        QCOMPARE(m_model->data(idx, ArtifactsModel::ToolNameRole).toString(),
                 QStringLiteral("generate_image"));
    }

    void test_imageMessage_withoutWorkspacePath_producesNoRow() {
        m_model->setActiveConversation(m_convId);
        Message m;
        m.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        m.conversationId = m_convId;
        m.role = QStringLiteral("assistant");
        m.content = QStringLiteral("Generated image.");
        m.finishReason = QStringLiteral("stop");
        m.createdAt = QDateTime::currentDateTimeUtc();
        m.metadata = QJsonObject{{QStringLiteral("produced_by"), QStringLiteral("image_service")}};
        m_msgSvc->addMessage(m);

        QTRY_COMPARE(m_model->rowCount(), 0);
    }

    void test_otherConversation_isIgnored() {
        m_model->setActiveConversation(m_convId);
        const QString otherConv = m_convSvc->createConversation(QStringLiteral("Other"));
        writeFileToolMessage(otherConv, QStringLiteral("/tmp/other.txt"));
        QTRY_COMPARE(m_model->rowCount(), 0);
    }

    void test_deleteMessage_removesArtifactRow() {
        m_model->setActiveConversation(m_convId);
        const QString id = writeFileToolMessage(m_convId, QStringLiteral("/tmp/bye.txt"));
        QTRY_COMPARE(m_model->rowCount(), 1);

        QVERIFY(m_msgSvc->deleteMessage(id));
        QTRY_COMPARE(m_model->rowCount(), 0);
    }

    void test_workspaceDir_listsSharedFiles_evenWithNoMessages() {
        const QString ws = m_tempDir.path() + QStringLiteral("/project-ws");
        QVERIFY(QDir().mkpath(ws + QStringLiteral("/images")));
        QFile a(ws + QStringLiteral("/report.md"));
        QVERIFY(a.open(QIODevice::WriteOnly));
        a.write("x");
        a.close();
        QFile b(ws + QStringLiteral("/images/hero.png"));
        QVERIFY(b.open(QIODevice::WriteOnly));
        b.write("y");
        b.close();

        m_model->setWorkspaceDir(ws);
        m_model->setActiveConversation(m_convId);

        QTRY_COMPARE(m_model->rowCount(), 2);
        QStringList names;
        for (int i = 0; i < m_model->rowCount(); ++i)
            names << m_model->data(m_model->index(i, 0), ArtifactsModel::FileNameRole).toString();
        names.sort();
        QCOMPARE(names, (QStringList{QStringLiteral("hero.png"), QStringLiteral("report.md")}));
    }

    void test_workspaceScan_prunesDependencyDirs() {
        const QString ws = m_tempDir.path() + QStringLiteral("/proj-deps");
        QVERIFY(QDir().mkpath(ws + QStringLiteral("/node_modules/react/lib")));
        QVERIFY(QDir().mkpath(ws + QStringLiteral("/.git")));
        QVERIFY(QDir().mkpath(ws + QStringLiteral("/src")));
        auto touch = [](const QString& p) -> bool {
            QFile f(p);
            if (!f.open(QIODevice::WriteOnly))
                return false;
            f.write("x");
            f.close();
            return true;
        };
        QVERIFY(touch(ws + QStringLiteral("/App.tsx")));
        QVERIFY(touch(ws + QStringLiteral("/src/index.ts")));
        QVERIFY(touch(ws + QStringLiteral("/node_modules/react/index.js")));
        QVERIFY(touch(ws + QStringLiteral("/node_modules/react/lib/react.js")));
        QVERIFY(touch(ws + QStringLiteral("/.git/config")));

        m_model->setWorkspaceDir(ws);
        m_model->setActiveConversation(m_convId);

        QTRY_COMPARE(m_model->rowCount(), 2);
        QStringList names;
        for (int i = 0; i < m_model->rowCount(); ++i)
            names << m_model->data(m_model->index(i, 0), ArtifactsModel::FileNameRole).toString();
        names.sort();
        QCOMPARE(names, (QStringList{QStringLiteral("App.tsx"), QStringLiteral("index.ts")}));
    }

    void test_workspaceDir_deduplicatesAgainstMessageRows() {
        const QString ws = m_tempDir.path() + QStringLiteral("/ws2");
        QVERIFY(QDir().mkpath(ws));
        const QString filePath = ws + QStringLiteral("/doc.txt");
        QFile f(filePath);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("z");
        f.close();

        m_model->setWorkspaceDir(ws);
        writeFileToolMessage(m_convId, filePath);
        m_model->setActiveConversation(m_convId);

        QTRY_COMPARE(m_model->rowCount(), 1);
        QCOMPARE(m_model->data(m_model->index(0, 0), ArtifactsModel::ToolNameRole).toString(),
                 QStringLiteral("write_file"));
    }

    void test_workspaceDir_relativeMessagePath_dedupsAgainstScan() {
        const QString ws = m_tempDir.path() + QStringLiteral("/wsrel");
        QVERIFY(QDir().mkpath(ws));
        QFile f(ws + QStringLiteral("/doc.txt"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("z");
        f.close();

        m_model->setWorkspaceDir(ws);
        writeFileToolMessage(m_convId, QStringLiteral("doc.txt"));
        m_model->setActiveConversation(m_convId);

        QTRY_COMPARE(m_model->rowCount(), 1);
        QCOMPARE(m_model->data(m_model->index(0, 0), ArtifactsModel::ToolNameRole).toString(),
                 QStringLiteral("write_file"));
    }

    void test_fileWrittenTwice_isListedOnce() {
        const QString ws = m_tempDir.path() + QStringLiteral("/wstwice");
        QVERIFY(QDir().mkpath(ws));
        QFile f(ws + QStringLiteral("/report.md"));
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("a");
        f.close();

        m_model->setWorkspaceDir(ws);
        writeFileToolMessage(m_convId, QStringLiteral("report.md"));
        writeFileToolMessage(m_convId, QStringLiteral("report.md"));
        m_model->setActiveConversation(m_convId);

        QTRY_COMPARE(m_model->rowCount(), 1);
    }
};

QTEST_MAIN(TestArtifactsModel)
#include "test-artifacts-model.moc"
