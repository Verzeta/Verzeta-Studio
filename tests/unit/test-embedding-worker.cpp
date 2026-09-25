// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "workers/embedding-worker.h"

#include <QtTest/QtTest>

class TestEmbeddingWorker : public QObject {
    Q_OBJECT

  private slots:
    void test_appendsEmbeddingsPath() {
        QCOMPARE(EmbeddingWorker::embeddingsUrl(QStringLiteral("https://api.openai.com/v1")),
                 QStringLiteral("https://api.openai.com/v1/embeddings"));
    }

    void test_idempotentWhenAlreadyFullUrl() {
        QCOMPARE(
            EmbeddingWorker::embeddingsUrl(QStringLiteral("https://api.openai.com/v1/embeddings")),
            QStringLiteral("https://api.openai.com/v1/embeddings"));
    }

    void test_trimsTrailingSlash() {
        QCOMPARE(EmbeddingWorker::embeddingsUrl(QStringLiteral("https://api.openai.com/v1/")),
                 QStringLiteral("https://api.openai.com/v1/embeddings"));
        QCOMPARE(EmbeddingWorker::embeddingsUrl(QStringLiteral("https://api.openai.com/v1///")),
                 QStringLiteral("https://api.openai.com/v1/embeddings"));
    }

    void test_localOllamaEndpoint() {
        QCOMPARE(EmbeddingWorker::embeddingsUrl(QStringLiteral("http://localhost:11434/v1")),
                 QStringLiteral("http://localhost:11434/v1/embeddings"));
    }
};

QTEST_GUILESS_MAIN(TestEmbeddingWorker)
#include "test-embedding-worker.moc"
