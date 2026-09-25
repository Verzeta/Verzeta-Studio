// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/models/db-manager.h"
#include "../../backend/models/job-context.h"
#include "../../backend/models/message.h"
#include "../../backend/services/conversation-service.h"
#include "../../backend/services/file-service.h"
#include "../../backend/services/image-service.h"
#include "../../backend/services/message-service.h"
#include "../../backend/services/model-router.h"

#include <QTemporaryDir>
#include <QtTest>

#include <QSqlDatabase>
#include <QStandardPaths>

class TestImageJobAttribution : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_dbDir;
    QString m_dbPath;

    std::unique_ptr<ModelRouter> m_router;
    std::unique_ptr<FileService> m_files;
    std::unique_ptr<ConversationService> m_convs;
    std::unique_ptr<MessageService> m_msgs;
    std::unique_ptr<ImageService> m_image;

    QString m_convA;
    QString m_convB;

    void fireImageReady(const JobContext& ctx, const QString& path) {
        QVERIFY(QMetaObject::invokeMethod(m_image.get(),
                                          "onImageReady",
                                          Qt::DirectConnection,
                                          Q_ARG(JobContext, ctx),
                                          Q_ARG(QString, path)));
    }

  private slots:
    void initTestCase() {
        qRegisterMetaType<JobContext>("JobContext");
        QStandardPaths::setTestModeEnabled(true);
    }

    void init() {
        QVERIFY(m_dbDir.isValid());
        m_dbPath =
            m_dbDir.path() + QStringLiteral("/test_%1.db").arg(QDateTime::currentMSecsSinceEpoch());

        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
        QVERIFY(DbManager::instance().open(m_dbPath));
        QVERIFY(DbManager::instance().runMigrations());

        m_router = std::make_unique<ModelRouter>();
        m_files = std::make_unique<FileService>();
        m_convs = std::make_unique<ConversationService>(DbManager::instance());
        m_msgs = std::make_unique<MessageService>(DbManager::instance());
        m_image = std::make_unique<ImageService>(*m_router, *m_files);
        m_image->setMessageService(m_msgs.get());

        m_convA = m_convs->createConversation(QStringLiteral("Conv A"));
        m_convB = m_convs->createConversation(QStringLiteral("Conv B"));
        QVERIFY(!m_convA.isEmpty());
        QVERIFY(!m_convB.isEmpty());
        QVERIFY(m_convA != m_convB);
    }

    void cleanup() {
        m_image.reset();
        m_msgs.reset();
        m_convs.reset();
        m_files.reset();
        m_router.reset();
        DbManager::instance().close();
        QSqlDatabase::removeDatabase(QStringLiteral("verzeta_main"));
    }


    void test_overlappingJobs_attribute_to_correct_conv() {
        const JobContext ctxA = JobContext::makeNew(m_convA, QStringLiteral("prompt-A"));
        const JobContext ctxB = JobContext::makeNew(m_convB, QStringLiteral("prompt-B"));

        QVERIFY(ctxA.isValid());
        QVERIFY(ctxB.isValid());
        QVERIFY(ctxA.jobId != ctxB.jobId);

        QCOMPARE(m_msgs->getRecentMessages(m_convA, 10).size(), 0);
        QCOMPARE(m_msgs->getRecentMessages(m_convB, 10).size(), 0);

        const QString pathB = m_dbDir.path() + QStringLiteral("/jobB.png");
        const QString pathA = m_dbDir.path() + QStringLiteral("/jobA.png");

        QFile fb(pathB);
        QVERIFY(fb.open(QIODevice::WriteOnly));
        fb.close();
        QFile fa(pathA);
        QVERIFY(fa.open(QIODevice::WriteOnly));
        fa.close();

        fireImageReady(ctxB, pathB);
        fireImageReady(ctxA, pathA);

        const auto msgsA = m_msgs->getRecentMessages(m_convA, 10);
        const auto msgsB = m_msgs->getRecentMessages(m_convB, 10);

        QCOMPARE(msgsA.size(), 1);
        QCOMPARE(msgsB.size(), 1);

        QCOMPARE(msgsA.first().conversationId, m_convA);
        QCOMPARE(msgsB.first().conversationId, m_convB);

        QVERIFY2(msgsA.first().content.contains(QStringLiteral("prompt-A")),
                 qPrintable(QStringLiteral("convA message should reference its own prompt; got: %1")
                                .arg(msgsA.first().content)));
        QVERIFY2(msgsB.first().content.contains(QStringLiteral("prompt-B")),
                 qPrintable(QStringLiteral("convB message should reference its own prompt; got: %1")
                                .arg(msgsB.first().content)));

        QCOMPARE(msgsA.first().metadata.value(QStringLiteral("job_id")).toString(), ctxA.jobId);
        QCOMPARE(msgsB.first().metadata.value(QStringLiteral("job_id")).toString(), ctxB.jobId);

        const QStringList imgsA = m_image->conversationImages(m_convA);
        const QStringList imgsB = m_image->conversationImages(m_convB);
        QCOMPARE(imgsA.size(), 1);
        QCOMPARE(imgsB.size(), 1);
        QCOMPARE(imgsA.first(), pathA);
        QCOMPARE(imgsB.first(), pathB);
    }

    void test_completedImage_filedIntoWorkspace() {
        const JobContext ctx =
            JobContext::makeNew(m_convA, QStringLiteral("Sunset Ice Cream Hero Banner!"));
        const QString src = m_dbDir.path() + QStringLiteral("/hero.png");
        QFile f(src);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("png-bytes");
        f.close();

        fireImageReady(ctx, src);

        const QString expected = m_files->activeProjectDir() +
                                 QStringLiteral("/images/sunset-ice-cream-hero-banner-") +
                                 ctx.jobId.left(8) + QStringLiteral(".png");
        QVERIFY2(QFile::exists(expected), qPrintable(expected));

        const auto msgs = m_msgs->getRecentMessages(m_convA, 10);
        QCOMPARE(msgs.size(), 1);
        QVERIFY2(msgs.first().content.contains(QStringLiteral("Saved to project files: images/")),
                 qPrintable(msgs.first().content));
    }

    void test_missingSource_stillPersistsMessage() {
        const JobContext ctx = JobContext::makeNew(m_convB, QStringLiteral("ghost"));
        fireImageReady(ctx, m_dbDir.path() + QStringLiteral("/nope.png"));
        const auto msgs = m_msgs->getRecentMessages(m_convB, 10);
        QCOMPARE(msgs.size(), 1);
        QVERIFY(!msgs.first().content.contains(QStringLiteral("Saved to project files")));
    }

    void test_invalidContext_dropped_safely() {
        const JobContext empty;
        QVERIFY(!empty.isValid());

        QCOMPARE(m_msgs->getRecentMessages(m_convA, 10).size(), 0);

        const QString path = m_dbDir.path() + QStringLiteral("/empty.png");
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.close();

        fireImageReady(empty, path);

        QCOMPARE(m_msgs->getRecentMessages(m_convA, 10).size(), 0);
        QCOMPARE(m_msgs->getRecentMessages(m_convB, 10).size(), 0);
    }

    void test_jobContext_factory_pins_basics() {
        const JobContext a = JobContext::makeNew(QStringLiteral("conv-1"), QStringLiteral("first"));
        const JobContext b = JobContext::makeNew(QStringLiteral("conv-1"), QStringLiteral("first"));

        QVERIFY(!a.jobId.isEmpty());
        QVERIFY(!b.jobId.isEmpty());
        QVERIFY(a.jobId != b.jobId);

        QCOMPARE(a.createdAt.timeSpec(), Qt::UTC);
        QVERIFY(a.createdAt.isValid());
        const qint64 ageMs = a.createdAt.msecsTo(QDateTime::currentDateTimeUtc());
        QVERIFY2(ageMs >= 0 && ageMs < 5000,
                 qPrintable(QStringLiteral("createdAt should be within "
                                           "5s; got age %1ms")
                                .arg(ageMs)));

        QVERIFY(a.requestingAlias.isEmpty());
        QVERIFY(a.requestingAgentId.isEmpty());

        QVERIFY(a.isValid());
        JobContext partial;
        partial.jobId = a.jobId;
        QVERIFY(!partial.isValid());
    }
};

QTEST_MAIN(TestImageJobAttribution)
#include "test-image-job-attribution.moc"
