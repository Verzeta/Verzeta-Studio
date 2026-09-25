// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file skill-hash.cpp
 * @brief Implementation of the deterministic skill folder hash.
 *        See skill-hash.h for the contract + algorithm.
 * @layer Utility
 * @dependencies Qt6::Core only.
 */

#include "skill-hash.h"

#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>

namespace SkillHash {

namespace {

constexpr qint64 kHashChunkBytes = 64 * 1024;  // 64 KiB per read

}  // namespace

QString computeFolderHash(const QString& folderPath, QString* outError) {
    QFileInfo rootInfo(folderPath);
    if (!rootInfo.exists() || !rootInfo.isDir()) {
        if (outError)
            *outError =
                QStringLiteral("folder does not exist or is not a directory: %1").arg(folderPath);
        return {};
    }

    const QString rootCanon = rootInfo.canonicalFilePath();

    // Walk recursively, collect relative POSIX paths of regular files.
    // Symlinks are deliberately ignored here — SkillParser rejects them
    // at the validation layer before this function runs. If somehow a
    // symlink survives to this point, QFileInfo::isFile() returns true
    // for the resolved target, so we'd hash the target's content. The
    // parser is the single chokepoint that prevents that.
    QStringList relativePaths;
    QDirIterator it(folderPath, QDir::Files | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString abs = it.next();
        const QString rel = QDir(rootCanon).relativeFilePath(abs);
        relativePaths.append(rel);
    }
    // Sort lex-ascending — case-sensitive (POSIX semantics). Determinism
    // depends on this step.
    std::sort(relativePaths.begin(), relativePaths.end());

    QCryptographicHash hash(QCryptographicHash::Sha256);

    for (const QString& rel : relativePaths) {
        const QString absPath = rootCanon + QStringLiteral("/") + rel;
        QFile f(absPath);
        if (!f.open(QIODevice::ReadOnly)) {
            if (outError)
                *outError = QStringLiteral("cannot open %1: %2").arg(rel, f.errorString());
            return {};
        }
        const qint64 size = f.size();

        // 1) relative path bytes
        hash.addData(rel.toUtf8());
        // 2) NUL separator
        hash.addData(QByteArray(1, '\0'));
        // 3) 8 bytes file size, little-endian
        QByteArray sizeBytes(8, '\0');
        for (int i = 0; i < 8; ++i) {
            sizeBytes[i] = static_cast<char>((size >> (8 * i)) & 0xff);
        }
        hash.addData(sizeBytes);
        // 4) NUL separator
        hash.addData(QByteArray(1, '\0'));
        // 5) file contents (chunked so we don't load whole file)
        while (!f.atEnd()) {
            hash.addData(f.read(kHashChunkBytes));
        }
        // 6) NUL separator (delimits this file from the next)
        hash.addData(QByteArray(1, '\0'));
        f.close();
    }

    return QString::fromLatin1(hash.result().toHex());
}

}  // namespace SkillHash
