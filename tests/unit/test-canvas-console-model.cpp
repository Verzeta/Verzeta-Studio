// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "models/canvas-console-model.h"

#include <QTest>

#include <QSignalSpy>

class TestCanvasConsoleModel : public QObject {
    Q_OBJECT

  private:
    std::unique_ptr<CanvasConsoleModel> m_model;

    QString text(int row) const {
        return m_model->data(m_model->index(row, 0), CanvasConsoleModel::TextRole).toString();
    }
    QString kind(int row) const {
        return m_model->data(m_model->index(row, 0), CanvasConsoleModel::KindRole).toString();
    }

  private slots:
    void init() { m_model = std::make_unique<CanvasConsoleModel>(); }
    void cleanup() { m_model.reset(); }

    void test_emptyState_isEmpty() {
        QCOMPARE(m_model->rowCount(), 0);
        QCOMPARE(m_model->count(), 0);
    }

    void test_runStarted_appendsSystemRow() {
        m_model->onRunStarted(QStringLiteral("hello.py"));
        QCOMPARE(m_model->rowCount(), 1);
        QCOMPARE(kind(0), QStringLiteral("system"));
        QVERIFY(text(0).contains(QStringLiteral("hello.py")));
    }

    void test_stdoutChunk_appendsStdoutRow() {
        m_model->onStdoutChunk(QStringLiteral("hello"));
        QCOMPARE(m_model->rowCount(), 1);
        QCOMPARE(kind(0), QStringLiteral("stdout"));
        QCOMPARE(text(0), QStringLiteral("hello"));
    }

    void test_stderrChunk_appendsStderrRow() {
        m_model->onStderrChunk(QStringLiteral("oops"));
        QCOMPARE(m_model->rowCount(), 1);
        QCOMPARE(kind(0), QStringLiteral("stderr"));
        QCOMPARE(text(0), QStringLiteral("oops"));
    }

    void test_runFinished_zero_emitsExitOk() {
        m_model->onRunFinished(0, 1234);
        QCOMPARE(m_model->rowCount(), 1);
        QCOMPARE(kind(0), QStringLiteral("exit-ok"));
        QVERIFY(text(0).contains(QStringLiteral("1234")));
    }

    void test_runFinished_nonzero_emitsExitErr() {
        m_model->onRunFinished(2, 800);
        QCOMPARE(kind(0), QStringLiteral("exit-err"));
        QVERIFY(text(0).contains(QStringLiteral("Exit 2")));
    }

    void test_runFinished_cancelled_emitsCancelMessage() {
        m_model->onRunFinished(-1, 100);
        QCOMPARE(kind(0), QStringLiteral("exit-err"));
        QVERIFY(text(0).contains(QStringLiteral("Cancel"), Qt::CaseInsensitive));
    }

    void test_runFinished_timedOut_emitsTimeout() {
        m_model->onRunFinished(-2, 10000);
        QCOMPARE(kind(0), QStringLiteral("exit-err"));
        QVERIFY(text(0).contains(QStringLiteral("Timed"), Qt::CaseInsensitive));
    }

    void test_failedToStart_emitsExitErr() {
        m_model->onRunFailedToStart(QStringLiteral("not found"));
        QCOMPARE(m_model->rowCount(), 1);
        QCOMPARE(kind(0), QStringLiteral("exit-err"));
        QVERIFY(text(0).contains(QStringLiteral("Failed")));
    }

    void test_clear_emptiesModelAndEmitsCount() {
        m_model->onStdoutChunk(QStringLiteral("a"));
        m_model->onStdoutChunk(QStringLiteral("b"));
        QSignalSpy spy(m_model.get(), &CanvasConsoleModel::countChanged);
        m_model->clear();
        QCOMPARE(m_model->rowCount(), 0);
        QCOMPARE(spy.count(), 1);
    }

    void test_fifoTrim_capsAtRowCap() {
        for (int i = 0; i < CanvasConsoleModel::kRowCap + 5; ++i) {
            m_model->onStdoutChunk(QStringLiteral("line %1").arg(i));
        }
        QCOMPARE(m_model->rowCount(), CanvasConsoleModel::kRowCap);
        QCOMPARE(text(0), QStringLiteral("line 5"));
        QCOMPARE(text(CanvasConsoleModel::kRowCap - 1),
                 QStringLiteral("line %1").arg(CanvasConsoleModel::kRowCap + 4));
    }

    void test_longLine_truncatedWithEllipsis() {
        QString longLine(CanvasConsoleModel::kMaxLineChars + 50, QLatin1Char('x'));
        m_model->onStdoutChunk(longLine);
        const QString got = text(0);
        QCOMPARE(got.size(), CanvasConsoleModel::kMaxLineChars + 1);
        QVERIFY(got.endsWith(QStringLiteral("…")));
    }

    void test_roleNames_exposeIsKindTextTimestamp() {
        const QHash<int, QByteArray> names = m_model->roleNames();
        QCOMPARE(names.value(CanvasConsoleModel::KindRole), QByteArrayLiteral("kind"));
        QCOMPARE(names.value(CanvasConsoleModel::TextRole), QByteArrayLiteral("text"));
        QCOMPARE(names.value(CanvasConsoleModel::TimestampRole), QByteArrayLiteral("timestamp"));
    }
};

QTEST_MAIN(TestCanvasConsoleModel)
#include "test-canvas-console-model.moc"
