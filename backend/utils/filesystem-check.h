// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file filesystem-check.h
 * @brief Detects whether a path lives on a network / distributed /
 *        VM-shared / FUSE-network filesystem so callers can refuse
 *        before SQLite (which does not support WAL over network
 *        filesystems) is allowed to open the file.
 * @layer Utility
 * @dependencies Qt6::Core
 */

#pragma once

#include <QString>

namespace Verzeta::Utils {

/**
 * @brief Summary of what kind of filesystem an absolute path is on,
 *        and whether that filesystem is safe for the engine database.
 *
 * `isNetwork` is true for filesystems that fail SQLite's locking
 * requirements (NFS, SMB / CIFS / Microsoft DFS, AFS, Ceph, Lustre,
 * GlusterFS, OCFS2, GFS2, BeeGFS, 9P / virtio-9p, VirtualBox /
 * VMware shared folders, SSHFS, WebDAV, S3FS, rclone-mount, …).
 * Pure-local filesystems (ext4, xfs, btrfs, APFS, NTFS, tmpfs,
 * overlay used by AppImage, ecryptfs / fscrypt, ntfs-3g, bindfs,
 * gocryptfs / cryfs / encfs) return false.
 */
struct FilesystemInfo {
    /** Short identifier (e.g. "nfs", "cifs", "fuse.sshfs", "ext4",
     *  "apfs", "DRIVE_REMOTE", "unknown"). Suitable for user-facing
     *  error messages. Never empty after a successful inspection. */
    QString typeName;

    /** True iff this filesystem is unsuitable for the engine DB. */
    bool isNetwork = false;

    /** Human-readable reason. Empty when isNetwork is false. */
    QString humanReason;
};

/**
 * @brief Inspects the filesystem at `absolutePath` and returns the
 *        verdict. Safe to call before the path actually exists; the
 *        nearest existing ancestor is used in that case.
 * @param absolutePath Absolute path to inspect. Empty paths return
 *        an "unknown" verdict with isNetwork = false.
 * @returns FilesystemInfo describing the type + verdict.
 */
FilesystemInfo inspectFilesystem(const QString& absolutePath);

// -----------------------------------------------------------------------
// Pure helpers — exposed for unit testing. The runtime path on each
// platform composes these with the OS-specific statfs / GetDriveType
// call; tests exercise the helpers directly without needing real
// network mounts.
// -----------------------------------------------------------------------

/**
 * @brief Classifies a Linux statfs(2) magic number. Returns false for
 *        ext*, xfs, btrfs, f2fs, ZFS, tmpfs, overlay, squashfs, ntfs3,
 *        and other local-block filesystems. Returns true for NFS,
 *        CIFS / SMB2, AFS, Coda, Ceph, Lustre, OCFS2, GFS2, BeeGFS,
 *        V9FS, vboxsf, and the catch-all FUSE magic (FUSE callers
 *        must then refine via mountinfo subtype).
 * @param magic The `f_type` field from a statfs(2) call.
 * @returns true if the magic identifies a known network / distributed
 *          / VM-share / generic-FUSE filesystem.
 */
bool isNetworkLinuxMagic(unsigned long magic);

/**
 * @brief Classifies a FUSE mount subtype (e.g. "sshfs", "rclone",
 *        "ntfs-3g", "gocryptfs"). Returns true for known
 *        network-FUSE; false for known local-FUSE; false for
 *        unrecognised subtypes (conservative: we want false
 *        positives on the local side, never on the network side).
 * @param subtype FUSE subtype string (case-insensitive, with or
 *        without "fuse." prefix).
 * @returns true if subtype identifies a known network-FUSE.
 */
bool isNetworkFuseSubtype(const QString& subtype);

/**
 * @brief Classifies a macOS statfs `f_fstypename` string.
 * @param fstypename The `f_fstypename` field from a statfs(2) call.
 * @returns true if the string identifies a known network filesystem
 *          on macOS (nfs, smbfs, afpfs, webdav, cifs, ftpfs).
 */
bool isNetworkMacFstypename(const QString& fstypename);

}  // namespace Verzeta::Utils
