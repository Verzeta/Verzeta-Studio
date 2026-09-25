// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file skill-archive-extractor.cpp
 * @brief Implementation of the safe ZIP extraction routine for
 *        ClawHub skill installation.  See skill-archive-extractor.h
 *        for the safety-cap contract; this file enforces every cap
 *        before writing a single entry to disk.
 * @layer Utility
 * @dependencies KF6::Archive (KZip, KArchiveDirectory, KArchiveFile,
 *               KArchiveEntry), Qt6::Core.
 */

#include "skill-archive-extractor.h"

#include <KArchiveDirectory>
#include <KArchiveEntry>
#include <KArchiveFile>
#include <KZip>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStack>

namespace SkillArchive {

namespace {

constexpr int kMaxFiles = 256;
constexpr qint64 kMaxSingleFileBytes = 1 * 1024 * 1024;
constexpr qint64 kMaxTotalUncompressedBytes = 8 * 1024 * 1024;

// Walk every leaf entry in the archive. Returns false on any safety
// violation. The KArchive API exposes nested directories; we flatten
// via a stack walk.
bool walk(const KArchiveDirectory* dir,
          const QString& relPathPrefix,
          const QString& stagingCanon,
          ExtractResult& out) {
    const auto entries = dir->entries();
    for (const QString& name : entries) {
        const KArchiveEntry* e = dir->entry(name);
        if (!e) {
            out.error = QStringLiteral("could not read the archive entry %1").arg(name);
            return false;
        }

        // Reject any explicit symlink. KArchive's entries-with-target
        // check via dynamic_cast to KArchiveFile + symLinkTarget()
        // returning non-empty.
        if (const auto* af = dynamic_cast<const KArchiveFile*>(e)) {
            if (!af->symLinkTarget().isEmpty()) {
                out.error = QStringLiteral("symbolic links are not allowed in skills: %1")
                                .arg(relPathPrefix + name);
                return false;
            }
        }

        // Compose normalized destination path.
        const QString relPath = relPathPrefix + name;
        if (relPath.contains(QStringLiteral(".."))) {
            out.error = QStringLiteral("path traversal: %1").arg(relPath);
            return false;
        }
        if (relPath.startsWith(QLatin1Char('/'))) {
            out.error = QStringLiteral("absolute path entry: %1").arg(relPath);
            return false;
        }
        const QString destAbs = QDir::cleanPath(stagingCanon + QStringLiteral("/") + relPath);
        if (!destAbs.startsWith(stagingCanon + QStringLiteral("/")) && destAbs != stagingCanon) {
            out.error = QStringLiteral("the archive contains a path outside the skill folder: %1")
                            .arg(relPath);
            return false;
        }

        if (e->isDirectory()) {
            const auto* sub = dynamic_cast<const KArchiveDirectory*>(e);
            if (!sub) {
                out.error = QStringLiteral("could not read a folder in the archive");
                return false;
            }
            QDir().mkpath(destAbs);
            if (!walk(sub, relPath + QStringLiteral("/"), stagingCanon, out)) {
                return false;
            }
            continue;
        }

        // Regular file.
        const auto* file = dynamic_cast<const KArchiveFile*>(e);
        if (!file)
            continue;
        ++out.fileCount;
        if (out.fileCount > kMaxFiles) {
            out.error = QStringLiteral("archive exceeds %1-file cap").arg(kMaxFiles);
            return false;
        }
        if (file->size() > kMaxSingleFileBytes) {
            out.error = QStringLiteral("file %1 exceeds 1 MiB cap").arg(relPath);
            return false;
        }
        out.uncompressedBytes += file->size();
        if (out.uncompressedBytes > kMaxTotalUncompressedBytes) {
            out.error = QStringLiteral("the archive is larger than 8 MiB "
                                       "when uncompressed");
            return false;
        }

        // Materialize parent directory + write file.
        QDir().mkpath(QFileInfo(destAbs).absolutePath());
        QFile out_f(destAbs);
        if (!out_f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            out.error = QStringLiteral("cannot open %1: %2").arg(destAbs, out_f.errorString());
            return false;
        }
        const QByteArray bytes = file->data();
        if (out_f.write(bytes) != bytes.size()) {
            out.error = QStringLiteral("short write to %1").arg(destAbs);
            return false;
        }
        out_f.close();
    }
    return true;
}

}  // namespace

ExtractResult extract(const QString& zipPath, const QString& stagingDir) {
    ExtractResult res;
    QFileInfo zfi(zipPath);
    if (!zfi.exists()) {
        res.error = QStringLiteral("zip not found: %1").arg(zipPath);
        return res;
    }
    if (!QDir().mkpath(stagingDir)) {
        res.error = QStringLiteral("cannot create staging: %1").arg(stagingDir);
        return res;
    }
    const QString stagingCanon = QFileInfo(stagingDir).canonicalFilePath();

    KZip zip(zipPath);
    if (!zip.open(QIODevice::ReadOnly)) {
        res.error = QStringLiteral("cannot open zip: %1").arg(zipPath);
        return res;
    }
    const KArchiveDirectory* root = zip.directory();
    if (!root) {
        res.error = QStringLiteral("zip has no root directory");
        return res;
    }

    if (!walk(root, QString(), stagingCanon, res)) {
        zip.close();
        return res;
    }
    zip.close();
    res.success = true;
    return res;
}

}  // namespace SkillArchive
