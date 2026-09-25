// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "services/file-service.h"

#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTest>
#include <QTextStream>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QUuid>


class TestFileService : public QObject {
    Q_OBJECT

  private:
    QTemporaryDir m_tmpDir;

  private slots:

    void initTestCase() {
        QVERIFY(m_tmpDir.isValid());
        QStandardPaths::setTestModeEnabled(true);
    }

    void cleanupTestCase() { QStandardPaths::setTestModeEnabled(false); }

    void test_pathSafety_traversalBlocked() {
        FileService svc;
        const QString base = m_tmpDir.path();

        QVERIFY(!svc.isPathSafe(QStringLiteral("/etc/passwd"), base));

        QVERIFY(!svc.isPathSafe(base + QStringLiteral("/../../etc/passwd"), base));
    }

    void test_pathSafety_normalPathAllowed() {
        FileService svc;
        const QString base = m_tmpDir.path();

        const QString safePath = base + QStringLiteral("/subdir/file.txt");
        QVERIFY(svc.isPathSafe(safePath, base));
    }

    void test_pathSafety_symlinkEscapeBlocked() {
        FileService svc;
        const QString base = m_tmpDir.path();

        const QString linkPath = base + QStringLiteral("/escapee");
        QFile::link(QStringLiteral("/tmp"), linkPath);

        QVERIFY(!svc.isPathSafe(linkPath, base));

        QFile::remove(linkPath);
    }

    void test_readFileContent_returnsCorrectBytes() {
        FileService svc;
        const QString path = m_tmpDir.path() + QStringLiteral("/hello.txt");
        const QByteArray expected = QByteArrayLiteral("Hello, World!\n");

        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(expected);
        f.close();

        const QByteArray actual = svc.readFileContent(path, 1024);
        QCOMPARE(actual, expected);
    }

    void test_readFileContent_maxBytesTruncates() {
        FileService svc;
        const QString path = m_tmpDir.path() + QStringLiteral("/large.bin");

        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QByteArray(200, 'X'));
        f.close();

        const QByteArray result = svc.readFileContent(path, 100);
        QCOMPARE(result.size(), 100);
    }

    void test_readFileContent_nonexistentEmitsError() {
        FileService svc;
        QSignalSpy errSpy(&svc, &FileService::error);

        const QByteArray data =
            svc.readFileContent(m_tmpDir.path() + QStringLiteral("/does-not-exist.txt"));

        QVERIFY(data.isEmpty());
        QCOMPARE(errSpy.count(), 1);
        QVERIFY(errSpy[0][0].toString().contains(QStringLiteral("not found")));
    }

    void test_saveGeneratedFile_writesCorrectContent() {
        FileService svc;
        QSignalSpy savedSpy(&svc, &FileService::fileSaved);

        const QString content = QStringLiteral("print('hello')\n");
        const QString savedPath =
            svc.saveGeneratedFile(QStringLiteral("hello.py"), content, m_tmpDir.path());

        QVERIFY(!savedPath.isEmpty());
        QCOMPARE(savedSpy.count(), 1);

        QFile f(savedPath);
        QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString readBack = QString::fromUtf8(f.readAll());
        f.close();
        QCOMPARE(readBack, content);
    }

    void test_saveGeneratedFile_createsDirectory() {
        FileService svc;

        const QString destDir = m_tmpDir.path() + QStringLiteral("/new-subdir/nested");
        QVERIFY(!QDir(destDir).exists());

        const QString savedPath =
            svc.saveGeneratedFile(QStringLiteral("test.txt"), QStringLiteral("content"), destDir);

        QVERIFY(!savedPath.isEmpty());
        QVERIFY(QDir(destDir).exists());
        QVERIFY(QFile::exists(savedPath));
    }

    void test_saveGeneratedFile_relativeSubdirPreserved() {
        FileService svc;
        const QString content = QStringLiteral("# spec\n");
        const QString savedPath =
            svc.saveGeneratedFile(QStringLiteral("drafts/spec.md"), content, m_tmpDir.path());

        QVERIFY(!savedPath.isEmpty());
        const QString expected = m_tmpDir.path() + QStringLiteral("/drafts/spec.md");
        QCOMPARE(QDir::cleanPath(savedPath), QDir::cleanPath(expected));
        QVERIFY(QFile::exists(expected));
        QVERIFY(!QFile::exists(m_tmpDir.path() + QStringLiteral("/draftsspec.md")));
    }

    void test_saveGeneratedFile_relativePathNormalised() {
        FileService svc;
        const QString savedPath = svc.saveGeneratedFile(
            QStringLiteral("a/b/../c.md"), QStringLiteral("x"), m_tmpDir.path());
        QVERIFY(!savedPath.isEmpty());
        QVERIFY(QFile::exists(m_tmpDir.path() + QStringLiteral("/a/c.md")));
    }

    void test_saveGeneratedFile_traversalRejected() {
        FileService svc;
        QSignalSpy errSpy(&svc, &FileService::error);
        const QString savedPath = svc.saveGeneratedFile(
            QStringLiteral("../escape.md"), QStringLiteral("x"), m_tmpDir.path());
        QVERIFY(savedPath.isEmpty());
        QCOMPARE(errSpy.count(), 1);
        QVERIFY(!QFile::exists(m_tmpDir.path() + QStringLiteral("/../escape.md")));
    }

    void test_saveGeneratedFile_absoluteContainedInDest() {
        FileService svc;
        const QString savedPath = svc.saveGeneratedFile(
            QStringLiteral("/etc/hostname"), QStringLiteral("x"), m_tmpDir.path());
        QVERIFY(!savedPath.isEmpty());
        QVERIFY(QDir::cleanPath(savedPath).startsWith(QDir::cleanPath(m_tmpDir.path())));
    }

    void test_sanitiseRelativePath_keepsSeparatorsSanitisesComponents() {
        QCOMPARE(FileService::sanitiseRelativePath(QStringLiteral("sub/fi:le.md")),
                 QStringLiteral("sub/file.md"));
        QCOMPARE(FileService::sanitiseRelativePath(QStringLiteral("a/b/../c")),
                 QStringLiteral("a/c"));
        QVERIFY(FileService::sanitiseRelativePath(QStringLiteral("../x")).isEmpty());
        QVERIFY(FileService::sanitiseRelativePath(QStringLiteral("/abs/x")).isEmpty());
    }

    void test_isWindowsReservedName() {
        QVERIFY(FileService::isWindowsReservedName(QStringLiteral("con")));
        QVERIFY(FileService::isWindowsReservedName(QStringLiteral("CON")));
        QVERIFY(FileService::isWindowsReservedName(QStringLiteral("con.txt")));
        QVERIFY(FileService::isWindowsReservedName(QStringLiteral("nul")));
        QVERIFY(FileService::isWindowsReservedName(QStringLiteral("com1")));
        QVERIFY(FileService::isWindowsReservedName(QStringLiteral("LPT9.log")));
        QVERIFY(FileService::isWindowsReservedName(QStringLiteral("aux.tar.gz")));
        QVERIFY(!FileService::isWindowsReservedName(QStringLiteral("console")));
        QVERIFY(!FileService::isWindowsReservedName(QStringLiteral("com0")));
        QVERIFY(!FileService::isWindowsReservedName(QStringLiteral("com10")));
        QVERIFY(!FileService::isWindowsReservedName(QStringLiteral("report.md")));
        QVERIFY(!FileService::isWindowsReservedName(QString()));
    }

    void test_mimeType_detectionCorrect() {
        FileService svc;

        const QString pyPath = m_tmpDir.path() + QStringLiteral("/script.py");
        {
            QFile f(pyPath);
            f.open(QIODevice::WriteOnly);
            f.write("#!/usr/bin/python3\nprint('hi')\n");
            f.close();
        }
        const QString pngPath = m_tmpDir.path() + QStringLiteral("/image.png");
        {
            QFile f(pngPath);
            f.open(QIODevice::WriteOnly);
            f.write(QByteArray::fromHex("89504e470d0a1a0a"));
            f.close();
        }

        const QString pyMime = svc.mimeType(pyPath);
        const QString pngMime = svc.mimeType(pngPath);

        QVERIFY2(pyMime.contains(QStringLiteral("python")) ||
                     pyMime.contains(QStringLiteral("text/x-script")) ||
                     pyMime.contains(QStringLiteral("text/plain")),
                 qPrintable(QStringLiteral("Unexpected python MIME: ") + pyMime));

        QVERIFY2(pngMime.contains(QStringLiteral("image/png")) ||
                     pngMime.contains(QStringLiteral("image/")),
                 qPrintable(QStringLiteral("Unexpected PNG MIME: ") + pngMime));
    }

    void test_storeAttachment_fileIsCopied() {
        FileService svc;

        const QString sourcePath = m_tmpDir.path() + QStringLiteral("/attach-source.txt");
        {
            QFile f(sourcePath);
            f.open(QIODevice::WriteOnly);
            f.write("attachment data");
            f.close();
        }

        const QString msgId = QUuid::createUuid().toString(QUuid::WithoutBraces);
        const QString relative = svc.storeAttachment(sourcePath, msgId);

        QVERIFY(!relative.isEmpty());
        QVERIFY(relative.contains(msgId));

        const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        const QString destPath = appData + QStringLiteral("/attachments/") + relative;
        QVERIFY(QFile::exists(destPath));
    }

    void test_createProjectBundle_zipContainsFiles() {
        if (!QFile::exists(QStringLiteral("/usr/bin/zip"))) {
            QSKIP("/usr/bin/zip not found — skipping bundle test");
        }

        FileService svc;
        QSignalSpy bundleSpy(&svc, &FileService::bundleCreated);

        const QMap<QString, QString> files = {
            {QStringLiteral("main.py"), QStringLiteral("print('hello')\n")},
            {QStringLiteral("README.md"), QStringLiteral("# Project\n")}};

        const QString zipPath = svc.createProjectBundle(files, QStringLiteral("my-project"));

        QVERIFY(!zipPath.isEmpty());
        QVERIFY(QFile::exists(zipPath));
        QCOMPARE(bundleSpy.count(), 1);

        const QFileInfo fi(zipPath);
        QVERIFY(fi.size() > 0);

        QFile::remove(zipPath);
    }

    void test_projectWorkspaceDir_isDeterministicAndContextFree() {
        FileService svc;
        const QString base = svc.activeProjectDir();
        const QString projId = QStringLiteral("09639fd4-1c91-4be0-85c7-7628e50e94cb");
        const QString proj =
            svc.projectWorkspaceDir(projId, QStringLiteral("Product Launch Plan 2"));

        QVERIFY(proj != base);
        QVERIFY(proj.startsWith(base + QStringLiteral("/")));
        QVERIFY(proj.endsWith(QStringLiteral("_09639fd4")));
        QVERIFY(QDir(proj).exists());

        svc.setActiveProjectContext(projId, QStringLiteral("Product Launch Plan 2"));
        QCOMPARE(svc.activeProjectDir(), proj);

        svc.setActiveConversation(QString());
        QCOMPARE(svc.projectWorkspaceDir(projId, QStringLiteral("Product Launch Plan 2")), proj);
    }

    void test_conversationWorkspaceDir_usesShortId() {
        FileService svc;
        const QString base = svc.activeProjectDir();
        const QString dir =
            svc.conversationWorkspaceDir(QStringLiteral("a93f9f06-005b-43ae-8f3c-7baaf936f9ca"));
        QVERIFY(dir != base);
        QVERIFY(dir.endsWith(QStringLiteral("/a93f9f06")));
        QVERIFY(QDir(dir).exists());
    }
};

QTEST_MAIN(TestFileService)
#include "test-file-service.moc"
