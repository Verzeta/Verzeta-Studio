// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "utils/logger.h"

#include <QTemporaryDir>
#include <QtTest>

#include <QFileInfo>

class TestLoggerRotation : public QObject {
    Q_OBJECT

  private slots:
    void rotatesAndKeepsOneBoundedBackup();
};

void TestLoggerRotation::rotatesAndKeepsOneBoundedBackup() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());

    const qint64 cap = 2000;
    Logger::setMaxLogBytesForTest(cap);
    Logger::initialize(dir.path());

    for (int i = 0; i < 300; ++i) {
        qCInfo(verzetaUi) << "logger rotation coverage line, padded for length" << i;
    }

    const QString current = dir.path() + QStringLiteral("/verzeta-studio.log");
    const QString backup = current + QStringLiteral(".1");

    QVERIFY2(QFileInfo::exists(backup),
             "verzeta-studio.log.1 should exist after the cap was crossed");
    QVERIFY2(QFileInfo(current).size() <= cap + 4096,
             "current log file should be bounded near the cap after rotation");
    QVERIFY2(!QFileInfo::exists(current + QStringLiteral(".2")),
             "only a single rotated backup (.1) should be retained");

    Logger::setMaxLogBytesForTest(5 * 1024 * 1024);
}

QTEST_GUILESS_MAIN(TestLoggerRotation)
#include "test-logger-rotation.moc"
