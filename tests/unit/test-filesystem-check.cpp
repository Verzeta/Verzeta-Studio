// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "utils/filesystem-check.h"

#include <QTemporaryDir>
#include <QtTest>

#include <QObject>
#include <QStandardPaths>

class TestFilesystemCheck : public QObject {
    Q_OBJECT

  private slots:
    void networkMagics_areAllRefused() {
        QVERIFY(Verzeta::Utils::isNetworkLinuxMagic(0x6969));
        QVERIFY(Verzeta::Utils::isNetworkLinuxMagic(0xFF534D42));
        QVERIFY(Verzeta::Utils::isNetworkLinuxMagic(0xFE534D42));
        QVERIFY(Verzeta::Utils::isNetworkLinuxMagic(0x517B));
        QVERIFY(Verzeta::Utils::isNetworkLinuxMagic(0x5346414F));
        QVERIFY(Verzeta::Utils::isNetworkLinuxMagic(0x73757245));
        QVERIFY(Verzeta::Utils::isNetworkLinuxMagic(0x00C36400));
        QVERIFY(Verzeta::Utils::isNetworkLinuxMagic(0x0BD00BD0));
        QVERIFY(Verzeta::Utils::isNetworkLinuxMagic(0x19830326));
        QVERIFY(Verzeta::Utils::isNetworkLinuxMagic(0x7461636F));
        QVERIFY(Verzeta::Utils::isNetworkLinuxMagic(0x01161970));
        QVERIFY(Verzeta::Utils::isNetworkLinuxMagic(0x01021997));
        QVERIFY(Verzeta::Utils::isNetworkLinuxMagic(0x786F4256));
        QVERIFY(Verzeta::Utils::isNetworkLinuxMagic(0xBACBACBC));
    }

    void localMagics_arePassed() {
        QVERIFY(!Verzeta::Utils::isNetworkLinuxMagic(0xEF53));
        QVERIFY(!Verzeta::Utils::isNetworkLinuxMagic(0x58465342));
        QVERIFY(!Verzeta::Utils::isNetworkLinuxMagic(0x9123683E));
        QVERIFY(!Verzeta::Utils::isNetworkLinuxMagic(0xF2F52010));
        QVERIFY(!Verzeta::Utils::isNetworkLinuxMagic(0x01021994));
        QVERIFY(!Verzeta::Utils::isNetworkLinuxMagic(0x794C7630));
        QVERIFY(!Verzeta::Utils::isNetworkLinuxMagic(0x73717368));
        QVERIFY(!Verzeta::Utils::isNetworkLinuxMagic(0xF15F));
    }

    void genericFuseMagic_returnsFalse_so_callerRefinesViaSubtype() {
        constexpr unsigned long FUSE_SUPER_MAGIC = 0x65735546;
        QVERIFY(!Verzeta::Utils::isNetworkLinuxMagic(FUSE_SUPER_MAGIC));
    }

    void networkFuseSubtypes_areRefused() {
        QVERIFY(Verzeta::Utils::isNetworkFuseSubtype(QStringLiteral("sshfs")));
        QVERIFY(Verzeta::Utils::isNetworkFuseSubtype(QStringLiteral("rclone")));
        QVERIFY(Verzeta::Utils::isNetworkFuseSubtype(QStringLiteral("s3fs")));
        QVERIFY(Verzeta::Utils::isNetworkFuseSubtype(QStringLiteral("gcsfuse")));
        QVERIFY(Verzeta::Utils::isNetworkFuseSubtype(QStringLiteral("cephfs")));
        QVERIFY(Verzeta::Utils::isNetworkFuseSubtype(QStringLiteral("glusterfs")));
        QVERIFY(Verzeta::Utils::isNetworkFuseSubtype(QStringLiteral("davfs")));
        QVERIFY(Verzeta::Utils::isNetworkFuseSubtype(QStringLiteral("ftpfs")));
        QVERIFY(Verzeta::Utils::isNetworkFuseSubtype(QStringLiteral("smbnetfs")));
    }

    void networkFuseSubtypes_withFusePrefix_areRefused() {
        QVERIFY(Verzeta::Utils::isNetworkFuseSubtype(QStringLiteral("fuse.sshfs")));
        QVERIFY(Verzeta::Utils::isNetworkFuseSubtype(QStringLiteral("fuse.rclone")));
        QVERIFY(Verzeta::Utils::isNetworkFuseSubtype(QStringLiteral("FUSE.SSHFS")));
    }

    void localFuseSubtypes_arePassed() {
        QVERIFY(!Verzeta::Utils::isNetworkFuseSubtype(QStringLiteral("ntfs-3g")));
        QVERIFY(!Verzeta::Utils::isNetworkFuseSubtype(QStringLiteral("bindfs")));
        QVERIFY(!Verzeta::Utils::isNetworkFuseSubtype(QStringLiteral("gocryptfs")));
        QVERIFY(!Verzeta::Utils::isNetworkFuseSubtype(QStringLiteral("cryfs")));
        QVERIFY(!Verzeta::Utils::isNetworkFuseSubtype(QStringLiteral("encfs")));
        QVERIFY(!Verzeta::Utils::isNetworkFuseSubtype(QStringLiteral("fuse.ntfs-3g")));
        QVERIFY(!Verzeta::Utils::isNetworkFuseSubtype(QStringLiteral("fuse-overlayfs")));
    }

    void unknownFuseSubtype_isRefusedConservatively() {
        QVERIFY(Verzeta::Utils::isNetworkFuseSubtype(QStringLiteral("some-novel-fuse")));
        QVERIFY(Verzeta::Utils::isNetworkFuseSubtype(QStringLiteral("fuse.exotic")));
    }

    void emptyFuseSubtype_isAllowed() {
        QVERIFY(!Verzeta::Utils::isNetworkFuseSubtype(QString()));
        QVERIFY(!Verzeta::Utils::isNetworkFuseSubtype(QStringLiteral("   ")));
    }

    void macNetworkFstypenames_areRefused() {
        QVERIFY(Verzeta::Utils::isNetworkMacFstypename(QStringLiteral("nfs")));
        QVERIFY(Verzeta::Utils::isNetworkMacFstypename(QStringLiteral("smbfs")));
        QVERIFY(Verzeta::Utils::isNetworkMacFstypename(QStringLiteral("cifs")));
        QVERIFY(Verzeta::Utils::isNetworkMacFstypename(QStringLiteral("afpfs")));
        QVERIFY(Verzeta::Utils::isNetworkMacFstypename(QStringLiteral("webdav")));
        QVERIFY(Verzeta::Utils::isNetworkMacFstypename(QStringLiteral("NFS")));
        QVERIFY(Verzeta::Utils::isNetworkMacFstypename(QStringLiteral(" smbfs ")));
    }

    void macLocalFstypenames_arePassed() {
        QVERIFY(!Verzeta::Utils::isNetworkMacFstypename(QStringLiteral("apfs")));
        QVERIFY(!Verzeta::Utils::isNetworkMacFstypename(QStringLiteral("hfs")));
        QVERIFY(!Verzeta::Utils::isNetworkMacFstypename(QStringLiteral("hfsplus")));
        QVERIFY(!Verzeta::Utils::isNetworkMacFstypename(QStringLiteral("exfat")));
        QVERIFY(!Verzeta::Utils::isNetworkMacFstypename(QStringLiteral("msdos")));
        QVERIFY(!Verzeta::Utils::isNetworkMacFstypename(QStringLiteral("tmpfs")));
    }

    void liveTempDir_classifiedAsLocal() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const auto info = Verzeta::Utils::inspectFilesystem(tmp.path());
        QVERIFY2(!info.isNetwork,
                 qPrintable(QStringLiteral("Temp dir classified as network: type=%1, reason=%2")
                                .arg(info.typeName, info.humanReason)));
        QVERIFY(!info.typeName.isEmpty());
    }

    void liveAbsentPath_classifiesViaExistingAncestor() {
        QTemporaryDir tmp;
        QVERIFY(tmp.isValid());
        const QString phantom =
            tmp.path() + QStringLiteral("/does/not/yet/exist/verzeta-studio.db");
        const auto info = Verzeta::Utils::inspectFilesystem(phantom);
        QVERIFY(!info.isNetwork);
        QVERIFY(!info.typeName.isEmpty());
    }

    void emptyPath_returnsUnknownAndLocal() {
        const auto info = Verzeta::Utils::inspectFilesystem(QString());
        QCOMPARE(info.typeName, QStringLiteral("unknown"));
        QVERIFY(!info.isNetwork);
    }
};

QTEST_GUILESS_MAIN(TestFilesystemCheck)
#include "test-filesystem-check.moc"
