// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file filesystem-check.cpp
 * @brief Cross-platform detection of network / distributed / VM-shared
 *        filesystems. SQLite WAL is documented (sqlite.org/wal.html)
 *        as not working over network filesystems; this utility lets
 *        callers refuse at startup with a clear error instead of
 *        suffering silent data corruption later.
 * @layer Utility
 * @dependencies Qt6::Core, POSIX statfs (Linux/macOS), Win32 API (Windows)
 */

#include "filesystem-check.h"

#include <QTextStream>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>

#if defined(Q_OS_LINUX)
#include <sys/statfs.h>
#include <sys/vfs.h>
#endif

#if defined(Q_OS_MACOS) || defined(Q_OS_BSD4)
#include <sys/mount.h>
#include <sys/param.h>
#endif

#if defined(Q_OS_WIN)
#include <windows.h>
#endif

namespace Verzeta::Utils {

namespace {

// -----------------------------------------------------------------------
// Linux magic-number table. Sourced from `linux/magic.h` and the
// per-filesystem kernel headers. Listed for explicit auditability;
// add new entries here when a new networked / distributed filesystem
// gains a stable magic number.
//
// IMPORTANT: this list is INTENTIONALLY conservative — we list only
// well-known network / distributed / VM-share filesystems. Anything
// not on this list (and not FUSE) is treated as local. New filesystems
// that emerge should be evaluated and added explicitly rather than
// caught by a heuristic.
// -----------------------------------------------------------------------
/**
 * @brief One row of the Linux statfs-magic deny-list.
 *
 * `magic` matches `struct statfs::f_type`; `name` is the short
 * user-facing identifier surfaced in the refusal error message.
 */
struct NetworkMagicEntry {
    unsigned long magic;
    const char* name;  // user-facing identifier
};

constexpr NetworkMagicEntry kNetworkMagics[] = {
    {0x6969, "nfs"},         // NFS_SUPER_MAGIC (all NFS versions)
    {0xFF534D42, "cifs"},    // CIFS_MAGIC_NUMBER (SMB v1; catches MS DFS)
    {0xFE534D42, "smb2"},    // SMB2_MAGIC_NUMBER (SMB2 / SMB3)
    {0x517B, "smb"},         // SMB_SUPER_MAGIC (legacy)
    {0x5346414F, "afs"},     // AFS_SUPER_MAGIC (OpenAFS, IBM AFS)
    {0x73757245, "coda"},    // CODA_SUPER_MAGIC
    {0x00C36400, "ceph"},    // CEPH_SUPER_MAGIC
    {0x0BD00BD0, "lustre"},  // LUSTRE_SUPER_MAGIC
    {0x19830326, "beegfs"},  // BEEGFS_SUPER_MAGIC
    {0x7461636F, "ocfs2"},   // OCFS2_SUPER_MAGIC (Oracle cluster)
    {0x01161970, "gfs2"},    // GFS2_MAGIC (Red Hat cluster)
    {0x01021997, "9p"},      // V9FS_MAGIC (Plan9 / virtio-9p / VM share)
    {0x786F4256, "vboxsf"},  // VBOXSF_SUPER_MAGIC (VirtualBox share)
    {0xBACBACBC, "vmhgfs"},  // VMware host-guest shared folders
};

constexpr unsigned long kFuseSuperMagic = 0x65735546;  // FUSE_SUPER_MAGIC

// -----------------------------------------------------------------------
// FUSE subtype classification. FUSE_SUPER_MAGIC alone is ambiguous
// (gocryptfs and sshfs both report it), so we refine via the mount
// subtype from /proc/self/mountinfo on Linux, getmntinfo() on macOS.
//
// REFUSE list — these are network / distributed / cloud-backed FUSE.
// ALLOW list — these are local-only translation / encryption FUSE.
// Anything not in either list defaults to REFUSE (conservative —
// we want to be loud about unknowns rather than silently corrupt).
// -----------------------------------------------------------------------
constexpr const char* kNetworkFuseSubtypes[] = {
    "sshfs",
    "rclone",
    "s3fs",
    "gcsfuse",
    "goofys",
    "cephfs",
    "glusterfs",
    "davfs",
    "webdavfs",
    "ftpfs",
    "curlftpfs",
    "smbnetfs",
    "fuse-smb",
    "fuse-cifs",
    "fuse-nfs",
    "afpfs",
};

constexpr const char* kLocalFuseSubtypes[] = {
    "ntfs-3g",
    "ntfs3",
    "bindfs",
    "gocryptfs",
    "cryfs",
    "encfs",
    "ecryptfs",
    "fuse-archive",
    "fuse-overlayfs",
    "rofs",
    "lklfuse",
    "fuseiso",
};

QString stripFusePrefix(QString s) {
    if (s.startsWith(QStringLiteral("fuse."), Qt::CaseInsensitive)) {
        s.remove(0, 5);
    }
    return s;
}

// -----------------------------------------------------------------------
// Walk `path` up the directory tree to the first existing ancestor.
// Used so we can inspect the filesystem even when the DB file or its
// parent dir hasn't been created yet.
// -----------------------------------------------------------------------
QString existingAncestor(const QString& path) {
    QString p = path;
    while (!p.isEmpty() && !QFileInfo::exists(p)) {
        const QFileInfo fi(p);
        const QString parent = fi.absolutePath();
        if (parent == p)
            break;  // reached root
        p = parent;
    }
    if (p.isEmpty())
        return QDir::rootPath();
    return p;
}

#if defined(Q_OS_LINUX)
// -----------------------------------------------------------------------
// Parse /proc/self/mountinfo to find the FUSE subtype for a given
// mount point. Returns empty if the path isn't on FUSE or the file
// can't be read.
//
// mountinfo format (per kernel docs Documentation/filesystems/proc.txt):
//   <mountid> <parentid> <major>:<minor> <root> <mountpoint> <options>
//   - <fstype> <source> <super-options>
//
// Field 9 (fstype) carries the FUSE subtype after a "fuse." prefix
// (e.g. "fuse.sshfs") for FUSE mounts.
// -----------------------------------------------------------------------
QString readFuseSubtype(const QString& path) {
    QFile mi(QStringLiteral("/proc/self/mountinfo"));
    if (!mi.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();

    // Find the longest mountpoint prefix that matches `path`.
    QString bestMount;
    QString bestSubtype;
    QTextStream in(&mi);
    while (!in.atEnd()) {
        const QString line = in.readLine();
        const QStringList parts = line.split(QLatin1Char(' '));
        // Need at least: mountid parentid dev root mountpoint options
        // sep(-) fstype source super
        if (parts.size() < 10)
            continue;
        const QString mountpoint = parts.value(4);
        // Find the literal "-" separator and pick the fstype after it.
        int sepIdx = -1;
        for (int i = 5; i < parts.size(); ++i) {
            if (parts.at(i) == QStringLiteral("-")) {
                sepIdx = i;
                break;
            }
        }
        if (sepIdx < 0 || sepIdx + 1 >= parts.size())
            continue;
        const QString fstype = parts.at(sepIdx + 1);

        if (path == mountpoint || path.startsWith(mountpoint + QLatin1Char('/'))) {
            if (mountpoint.size() > bestMount.size()) {
                bestMount = mountpoint;
                bestSubtype = fstype;
            }
        }
    }
    return bestSubtype;
}
#endif  // Q_OS_LINUX

}  // namespace

// -----------------------------------------------------------------------
// Pure helpers
// -----------------------------------------------------------------------

bool isNetworkLinuxMagic(unsigned long magic) {
    for (const auto& entry : kNetworkMagics) {
        if (entry.magic == magic)
            return true;
    }
    // Generic FUSE: caller must refine via subtype. Returning true
    // here would over-refuse local FUSE (ntfs-3g, gocryptfs, etc.);
    // returning false here would under-refuse network-FUSE (sshfs).
    // The cpp-side runtime resolves this by checking subtype before
    // the verdict; the pure helper just reports "is this a known
    // network magic", which FUSE alone is NOT.
    Q_UNUSED(kFuseSuperMagic);
    return false;
}

bool isNetworkFuseSubtype(const QString& subtype) {
    const QString s = stripFusePrefix(subtype.trimmed().toLower());
    if (s.isEmpty())
        return false;
    for (const char* net : kNetworkFuseSubtypes) {
        if (s == QString::fromLatin1(net))
            return true;
    }
    for (const char* local : kLocalFuseSubtypes) {
        if (s == QString::fromLatin1(local))
            return false;
    }
    // Conservative default for unknown subtypes: refuse. A new
    // user-facing FUSE that's actually local can be added to
    // kLocalFuseSubtypes; better to be loud than silently corrupt.
    return true;
}

bool isNetworkMacFstypename(const QString& fstypename) {
    const QString s = fstypename.trimmed().toLower();
    return s == QStringLiteral("nfs") || s == QStringLiteral("smbfs") ||
           s == QStringLiteral("cifs") || s == QStringLiteral("afpfs") ||
           s == QStringLiteral("webdav") || s == QStringLiteral("ftpfs") ||
           s == QStringLiteral("sshfs");
}

// -----------------------------------------------------------------------
// Public entry point
// -----------------------------------------------------------------------

FilesystemInfo inspectFilesystem(const QString& absolutePath) {
    FilesystemInfo info;
    info.typeName = QStringLiteral("unknown");

    if (absolutePath.isEmpty())
        return info;

    const QString target = existingAncestor(absolutePath);

#if defined(Q_OS_LINUX)
    struct statfs buf;
    if (::statfs(target.toLocal8Bit().constData(), &buf) != 0) {
        // Statfs failed — report unknown but do not refuse. Caller
        // will likely fail to open the path anyway and report that.
        return info;
    }
    const unsigned long magic = static_cast<unsigned long>(buf.f_type);

    // Generic FUSE — must check the subtype via mountinfo.
    if (magic == kFuseSuperMagic) {
        const QString subtype = readFuseSubtype(target);
        if (subtype.isEmpty()) {
            info.typeName = QStringLiteral("fuse (subtype unknown)");
            info.isNetwork = true;  // conservative
            info.humanReason =
                QStringLiteral("FUSE filesystem with unrecognised subtype, treating it as "
                               "network to avoid silent corruption.");
            return info;
        }
        info.typeName = QStringLiteral("fuse.") + stripFusePrefix(subtype);
        if (isNetworkFuseSubtype(subtype)) {
            info.isNetwork = true;
            info.humanReason =
                QStringLiteral("FUSE subtype \"%1\" is a network / cloud-backed mount; "
                               "SQLite WAL is not supported.")
                    .arg(stripFusePrefix(subtype));
        }
        return info;
    }

    // Other magic — walk the deny-list.
    for (const auto& entry : kNetworkMagics) {
        if (entry.magic == magic) {
            info.typeName = QString::fromLatin1(entry.name);
            info.isNetwork = true;
            info.humanReason =
                QStringLiteral("Filesystem \"%1\" is a network / distributed / VM-shared "
                               "mount; SQLite WAL is not supported.")
                    .arg(info.typeName);
            return info;
        }
    }

    // Local — best-effort typeName from a small allow-list for nicer
    // diagnostics. Unknown locals still pass through.
    switch (magic) {
        case 0xEF53:
            info.typeName = QStringLiteral("ext");
            break;
        case 0x58465342:
            info.typeName = QStringLiteral("xfs");
            break;
        case 0x9123683E:
            info.typeName = QStringLiteral("btrfs");
            break;
        case 0xF2F52010:
            info.typeName = QStringLiteral("f2fs");
            break;
        case 0x2FC12FC1:
            info.typeName = QStringLiteral("zfs");
            break;
        case 0x01021994:
            info.typeName = QStringLiteral("tmpfs");
            break;
        case 0x794C7630:
            info.typeName = QStringLiteral("overlay");
            break;
        case 0x73717368:
            info.typeName = QStringLiteral("squashfs");
            break;
        case 0x5346544E:
            info.typeName = QStringLiteral("ntfs3");
            break;
        case 0x2011BAB0:
            info.typeName = QStringLiteral("exfat");
            break;
        case 0xF15F:
            info.typeName = QStringLiteral("ecryptfs");
            break;
        default:
            info.typeName = QStringLiteral("local");
            break;
    }
    return info;

#elif defined(Q_OS_MACOS) || defined(Q_OS_BSD4)
    struct statfs buf;
    if (::statfs(target.toLocal8Bit().constData(), &buf) != 0) {
        return info;
    }
    const QString fstype = QString::fromLocal8Bit(buf.f_fstypename);

    if (isNetworkMacFstypename(fstype)) {
        info.typeName = fstype;
        info.isNetwork = true;
        info.humanReason =
            QStringLiteral("Filesystem \"%1\" is a network mount; SQLite WAL is not supported.")
                .arg(fstype);
        return info;
    }

    // FUSE on macOS — refine via subtype if recognisable.
    const QString lower = fstype.toLower();
    if (lower.contains(QStringLiteral("fuse"))) {
        info.typeName = fstype;
        // Check for known network FUSE names embedded in the type.
        for (const char* net : kNetworkFuseSubtypes) {
            const QString n = QString::fromLatin1(net);
            if (lower.contains(n)) {
                info.isNetwork = true;
                info.humanReason =
                    QStringLiteral("FUSE mount \"%1\" is a network / cloud-backed mount; "
                                   "SQLite WAL is not supported.")
                        .arg(fstype);
                return info;
            }
        }
        // Unknown FUSE — conservative refusal.
        info.isNetwork = true;
        info.humanReason =
            QStringLiteral("FUSE mount \"%1\" has an unrecognised type, treating it as "
                           "network to avoid silent corruption.")
                .arg(fstype);
        return info;
    }

    info.typeName = fstype.isEmpty() ? QStringLiteral("local") : fstype;
    return info;

#elif defined(Q_OS_WIN)
    // Walk up to the volume root.
    const std::wstring wpath = target.toStdWString();
    wchar_t volumeRoot[MAX_PATH] = {0};
    if (!::GetVolumePathNameW(wpath.c_str(), volumeRoot, MAX_PATH)) {
        return info;
    }
    const UINT driveType = ::GetDriveTypeW(volumeRoot);
    switch (driveType) {
        case DRIVE_REMOTE:
            info.typeName = QStringLiteral("DRIVE_REMOTE");
            info.isNetwork = true;
            info.humanReason =
                QStringLiteral("Drive is a remote / network share (SMB, DFS, NetWare, or "
                               "mapped network drive); SQLite WAL is not supported.");
            return info;
        case DRIVE_CDROM:
            info.typeName = QStringLiteral("DRIVE_CDROM");
            info.isNetwork = true;
            info.humanReason =
                QStringLiteral("Drive is read-only (CD/DVD/BD); cannot host an engine database.");
            return info;
        case DRIVE_UNKNOWN:
        case DRIVE_NO_ROOT_DIR:
            info.typeName = QStringLiteral("DRIVE_UNKNOWN");
            info.isNetwork = true;
            info.humanReason =
                QStringLiteral("Drive type could not be determined, refusing conservatively.");
            return info;
        case DRIVE_FIXED:
            info.typeName = QStringLiteral("DRIVE_FIXED");
            break;
        case DRIVE_RAMDISK:
            info.typeName = QStringLiteral("DRIVE_RAMDISK");
            break;
        case DRIVE_REMOVABLE:
            info.typeName = QStringLiteral("DRIVE_REMOVABLE");
            break;
        default:
            info.typeName = QStringLiteral("DRIVE_OTHER");
            break;
    }
    return info;

#else
    // Unsupported platform — pass through. Better to allow than to
    // wedge on a platform we haven't characterised.
    info.typeName = QStringLiteral("unsupported-platform");
    return info;
#endif
}

}  // namespace Verzeta::Utils
