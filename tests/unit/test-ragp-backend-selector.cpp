// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "services/ragp/ragp-backend-selector.h"

#include <QTemporaryDir>
#include <QtTest>

#include <QFile>

class TestRagpBackendSelector : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tempDir;

    QString makeFile(const QString& name) {
        const QString path = m_tempDir.filePath(name);
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return QString();
        f.write("fake gguf bytes");
        f.close();
        return path;
    }

  private slots:

    void initTestCase() {
        QVERIFY2(m_tempDir.isValid(), "QTemporaryDir could not be created — host filesystem issue");
    }


    void testRemoteWhenLocalDisabled() {
        const QString path = makeFile(QStringLiteral("valid.gguf"));
        QVERIFY(!path.isEmpty());
        QCOMPARE(Ragp::chooseRagpBackendSpec(false, path), QStringLiteral("remote"));
    }

    void testRemoteWhenLocalDisabledAndPathEmpty() {
        QCOMPARE(Ragp::chooseRagpBackendSpec(false, QString{}), QStringLiteral("remote"));
    }

    void testRemoteWhenLocalEnabledButPathEmpty() {
        QCOMPARE(Ragp::chooseRagpBackendSpec(true, QString{}), QStringLiteral("remote"));
    }

    void testRemoteWhenFileDoesNotExist() {
#ifdef VERZETA_HAS_LLAMA
        const QString bogus = m_tempDir.filePath(QStringLiteral("gone.gguf"));
        QCOMPARE(Ragp::chooseRagpBackendSpec(true, bogus), QStringLiteral("remote"));
#else
        QCOMPARE(Ragp::chooseRagpBackendSpec(true, QStringLiteral("/any/path.gguf")),
                 QStringLiteral("remote"));
#endif
    }

#ifdef VERZETA_HAS_LLAMA
    void testRemoteWhenPathIsDirectory() {
        const QString dirPath = m_tempDir.filePath(QStringLiteral("dirmodel.gguf"));
        QVERIFY(QDir().mkpath(dirPath));
        QCOMPARE(Ragp::chooseRagpBackendSpec(true, dirPath), QStringLiteral("remote"));
    }
#endif


#ifdef VERZETA_HAS_LLAMA
    void testLocalWhenAllConditionsMet() {
        const QString path = makeFile(QStringLiteral("phi-4.gguf"));
        QVERIFY(!path.isEmpty());
        const QString spec = Ragp::chooseRagpBackendSpec(true, path);
        QVERIFY2(spec.startsWith(QStringLiteral("local:")), qPrintable(spec));
        QCOMPARE(spec.mid(QStringLiteral("local:").size()), path);
    }

    void testLocalPathRoundTripsAcrossCalls() {
        const QString path = makeFile(QStringLiteral("repeat.gguf"));
        const QString first = Ragp::chooseRagpBackendSpec(true, path);
        const QString second = Ragp::chooseRagpBackendSpec(true, path);
        QCOMPARE(first, second);
    }
#else
    void testNoLlamaBuildAlwaysReturnsRemote() {
        QCOMPARE(Ragp::chooseRagpBackendSpec(true, QStringLiteral("/any/path.gguf")),
                 QStringLiteral("remote"));
    }
#endif
};

QTEST_APPLESS_MAIN(TestRagpBackendSelector)
#include "test-ragp-backend-selector.moc"
