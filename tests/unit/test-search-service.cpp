// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/db-manager.h"
#include "services/conversation-service.h"
#include "services/message-service.h"
#include "services/rag-service.h"
#include "services/search-service.h"
#include "workers/embedding-worker.h"

#include <QTemporaryDir>
#include <QTest>

#include <QDateTime>
#include <QObject>
#include <QSignalSpy>
#include <QSqlDatabase>
#include <QStandardPaths>
#include <QUuid>


class TestSearchService : public QObject {
    Q_OBJECT

  private:
    DbManager* m_db = nullptr;
    ConversationService* m_convSvc = nullptr;
    MessageService* m_msgSvc = nullptr;
    EmbeddingWorker* m_embWorker = nullptr;
    RagService* m_ragService = nullptr;
    SearchService* m_searchService = nullptr;
    QTemporaryDir m_tempDir;

    QString m_convId;
    QString m_msg1Id;
    QString m_msg2Id;
    QString m_msg3Id;

  private slots:
    void initTestCase() {
        QVERIFY(m_tempDir.isValid());
        QStandardPaths::setTestModeEnabled(true);
    }

    void init() {
        m_db = &DbManager::instance();
        const QString dbPath = m_tempDir.filePath(QStringLiteral("test-search.db"));
        QVERIFY(m_db->open(dbPath));
        QVERIFY(m_db->runMigrations());

        m_convSvc = new ConversationService(*m_db, this);
        m_msgSvc = new MessageService(*m_db, this);
        m_embWorker = new EmbeddingWorker(this);
        m_ragService = new RagService(*m_db, *m_embWorker, this);
        m_searchService = new SearchService(*m_db, *m_ragService, this);

        m_convId = m_convSvc->createConversation(QStringLiteral("Test Conversation"));
        QVERIFY(!m_convId.isEmpty());

        Message m1;
        m1.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        m1.conversationId = m_convId;
        m1.role = QStringLiteral("user");
        m1.content = QStringLiteral("Hello world, how are you today?");
        m1.createdAt = QDateTime::currentDateTimeUtc();
        m_msg1Id = m_msgSvc->addMessage(m1);
        QVERIFY(!m_msg1Id.isEmpty());

        Message m2;
        m2.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        m2.conversationId = m_convId;
        m2.role = QStringLiteral("assistant");
        m2.content = QStringLiteral("I'm doing great! The weather is wonderful today.");
        m2.createdAt = QDateTime::currentDateTimeUtc();
        m_msg2Id = m_msgSvc->addMessage(m2);
        QVERIFY(!m_msg2Id.isEmpty());

        Message m3;
        m3.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        m3.conversationId = m_convId;
        m3.role = QStringLiteral("user");
        m3.content = QStringLiteral("Can you explain quantum computing to me?");
        m3.createdAt = QDateTime::currentDateTimeUtc();
        m_msg3Id = m_msgSvc->addMessage(m3);
        QVERIFY(!m_msg3Id.isEmpty());
    }

    void cleanup() {
        delete m_searchService;
        m_searchService = nullptr;
        delete m_ragService;
        m_ragService = nullptr;
        delete m_embWorker;
        m_embWorker = nullptr;
        delete m_msgSvc;
        m_msgSvc = nullptr;
        delete m_convSvc;
        m_convSvc = nullptr;
        m_db->close();
    }


    void test_ftsExactMatch() {
        const auto results = m_searchService->searchFullText(QStringLiteral("world"));
        QVERIFY(!results.isEmpty());
        const bool found =
            std::any_of(results.begin(), results.end(), [this](const SearchResult& r) {
                return r.messageId == m_msg1Id;
            });
        QVERIFY(found);
    }

    void test_ftsPhraseSearch() {
        const auto results = m_searchService->searchFullText(QStringLiteral("\"hello world\""));
        QVERIFY(!results.isEmpty());
        const bool found =
            std::any_of(results.begin(), results.end(), [this](const SearchResult& r) {
                return r.messageId == m_msg1Id;
            });
        QVERIFY(found);
    }

    void test_ftsNoResults() {
        const auto results = m_searchService->searchFullText(QStringLiteral("xyzzynotaword"));
        QVERIFY(results.isEmpty());
    }

    void test_ftsResultHasConversationTitle() {
        const auto results = m_searchService->searchFullText(QStringLiteral("quantum"));
        QVERIFY(!results.isEmpty());
        QCOMPARE(results.first().conversationTitle, QStringLiteral("Test Conversation"));
    }

    void test_ftsResultHasSnippet() {
        const auto results = m_searchService->searchFullText(QStringLiteral("weather"));
        QVERIFY(!results.isEmpty());
        QVERIFY(!results.first().snippet.isEmpty());
    }

    void test_ftsResultRole() {
        const auto results = m_searchService->searchFullText(QStringLiteral("quantum"));
        QVERIFY(!results.isEmpty());
        QCOMPARE(results.first().role, QStringLiteral("user"));
    }

    void test_ftsResultTimestamp() {
        const auto results = m_searchService->searchFullText(QStringLiteral("world"));
        QVERIFY(!results.isEmpty());
        QVERIFY(results.first().timestamp.isValid());
    }

    void test_snippetHighlighting() {
        const auto results = m_searchService->searchFullText(QStringLiteral("world"));
        QVERIFY(!results.isEmpty());
        const QString snip = results.first().snippet;
        QVERIFY(snip.contains(QStringLiteral("world"), Qt::CaseInsensitive));
    }

    void test_indexMessageManual() {
        const QString manualId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        Message m;
        m.id = manualId;
        m.conversationId = m_convId;
        m.role = QStringLiteral("user");
        m.content = QStringLiteral("original content without special words");
        m.createdAt = QDateTime::currentDateTimeUtc();
        QVERIFY(!m_msgSvc->addMessage(m).isEmpty());

        m_searchService->indexMessage(
            manualId, m_convId, QStringLiteral("unique-keyword-zephyr-sunshine"));

        const auto results = m_searchService->searchFullText(QStringLiteral("zephyr"));
        QVERIFY(!results.isEmpty());
        const bool found =
            std::any_of(results.begin(), results.end(), [&manualId](const SearchResult& r) {
                return r.messageId == manualId;
            });
        QVERIFY(found);
    }

    void test_ftsLimitRespected() {
        const auto results = m_searchService->searchFullText(QStringLiteral("today"), 1);
        QVERIFY(results.size() <= 1);
    }

    void test_combinedSearchReturnsFTSResults() {
        QSignalSpy spy(m_searchService, &SearchService::searchResultsReady);
        m_searchService->searchCombined(QStringLiteral("world"), 20);
        if (spy.isEmpty()) {
            QVERIFY(spy.wait(5000));
        }
        QCOMPARE(spy.count(), 1);
        const QVariantList results = spy.at(0).at(0).toList();
        QVERIFY(!results.isEmpty());
    }

    void test_crossConversationSearch() {
        const QString conv2Id =
            m_convSvc->createConversation(QStringLiteral("Second Conversation"));
        QVERIFY(!conv2Id.isEmpty());

        Message m4;
        m4.id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        m4.conversationId = conv2Id;
        m4.role = QStringLiteral("user");
        m4.content = QStringLiteral("Explain photosynthesis clearly");
        m4.createdAt = QDateTime::currentDateTimeUtc();
        const QString msg4Id = m_msgSvc->addMessage(m4);
        QVERIFY(!msg4Id.isEmpty());

        const auto results = m_searchService->searchFullText(QStringLiteral("photosynthesis"));
        QVERIFY(!results.isEmpty());
        QCOMPARE(results.first().conversationId, conv2Id);
    }
};

QTEST_MAIN(TestSearchService)
#include "test-search-service.moc"
