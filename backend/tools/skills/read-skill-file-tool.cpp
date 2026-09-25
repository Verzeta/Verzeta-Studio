// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file read-skill-file-tool.cpp
 * @brief Implementation of the `read_skill_file` tool body.
 *
 *        Enforces path containment, no-symlinks, the on-disk byte cap,
 *        NUL-byte binary detection, and LLM-return truncation.
 * @layer Service (Tool subsystem)
 * @dependencies SkillService, Qt6::Core (QDir / QFile / QFileInfo).
 */

#include "read-skill-file-tool.h"

#include "../../services/skill-service.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace Tools {

namespace {
constexpr qint64 kOnDiskCapBytes = 256 * 1024;
constexpr int kLlmReturnCap = 32 * 1024;
constexpr int kBinarySniffSize = 8 * 1024;
}  // namespace

QString ReadSkillFileTool::name() const {
    return QStringLiteral("read_skill_file");
}

QString ReadSkillFileTool::description() const {
    return QStringLiteral("Read a supporting text file inside an approved skill (e.g. "
                          "examples/run.sh, README.md). Path is relative to the skill "
                          "root; path-traversal is rejected. Binary files and files over "
                          "256 KiB are refused.");
}

QList<ToolParameterSchema> ReadSkillFileTool::parameters() const {
    ToolParameterSchema id;
    id.name = QStringLiteral("skill_id");
    id.type = QStringLiteral("string");
    id.description = QStringLiteral("The skill id.");
    id.required = true;

    ToolParameterSchema rel;
    rel.name = QStringLiteral("relative_path");
    rel.type = QStringLiteral("string");
    rel.description = QStringLiteral("Relative path inside the skill root. Must NOT contain `..`, "
                                     "must NOT be absolute, must NOT cross a symlink.");
    rel.required = true;

    return {id, rel};
}

QJsonValue ReadSkillFileTool::invoke(const QJsonObject& args) {
    if (!m_deps.isValid()) {
        return QJsonObject{
            {QStringLiteral("error"), QStringLiteral("skill tool deps not configured")}};
    }
    // Args-injected conv id wins over the captured-LOCAL getter so
    // wire-side per-client cascades resolve skill scope against
    // THEIR conversation's folder / project, not the LOCAL CC's.
    QString activeConv = args.value(QStringLiteral("__caller_conv_id")).toString();
    if (activeConv.isEmpty())
        activeConv = m_deps.activeConvIdGetter();
    if (activeConv.isEmpty()) {
        return QJsonObject{
            {QStringLiteral("error"), QStringLiteral("no active responder context")}};
    }
    const QString skillId = args.value(QStringLiteral("skill_id")).toString();
    const QString rel = args.value(QStringLiteral("relative_path")).toString();
    if (skillId.isEmpty() || rel.isEmpty()) {
        return QJsonObject{
            {QStringLiteral("error"), QStringLiteral("skill_id and relative_path required")}};
    }

    // Approval + scope checks.
    const Skill s = m_deps.skills->skillById(skillId);
    if (!s.isValid())
        return QJsonObject{{QStringLiteral("error"), QStringLiteral("skill not installed")}};
    if (!s.isApproved())
        return QJsonObject{{QStringLiteral("error"), QStringLiteral("skill is not approved")}};
    const auto resolved = m_deps.skills->resolveForConversation(activeConv);
    if (resolved.exposeOnly && !resolved.preferredSkillIds.contains(skillId)) {
        return QJsonObject{{QStringLiteral("error"),
                            QStringLiteral("skill not in preferred list (exposeOnly active)")}};
    }

    // Relative-path grammar.
    if (rel.contains(QStringLiteral("..")) || rel.startsWith(QLatin1Char('/')) ||
        rel.contains(QChar('\\')) || rel.contains(QChar('\0'))) {
        return QJsonObject{{QStringLiteral("error"), QStringLiteral("invalid relative path")}};
    }

    const QString rootAbs = m_deps.skills->installPathFor(skillId);
    if (rootAbs.isEmpty()) {
        return QJsonObject{{QStringLiteral("error"), QStringLiteral("skill root not found")}};
    }
    const QString fileAbs = QDir::cleanPath(rootAbs + QStringLiteral("/") + rel);
    const QString rootCanon = QFileInfo(rootAbs).canonicalFilePath();

    // Path containment.
    QFileInfo fi(fileAbs);
    if (!fi.exists()) {
        return QJsonObject{{QStringLiteral("error"), QStringLiteral("file not found")}};
    }
    if (fi.isSymLink()) {
        return QJsonObject{{QStringLiteral("error"), QStringLiteral("symlink not permitted")}};
    }
    if (!fi.canonicalFilePath().startsWith(rootCanon + QStringLiteral("/")) &&
        fi.canonicalFilePath() != rootCanon) {
        return QJsonObject{{QStringLiteral("error"), QStringLiteral("path escapes skill root")}};
    }
    if (fi.size() > kOnDiskCapBytes) {
        return QJsonObject{{QStringLiteral("error"), QStringLiteral("file too large")}};
    }

    QFile f(fileAbs);
    if (!f.open(QIODevice::ReadOnly)) {
        return QJsonObject{{QStringLiteral("error"), QStringLiteral("cannot open file")}};
    }
    const QByteArray sniff = f.read(kBinarySniffSize);
    if (sniff.contains('\0')) {
        return QJsonObject{{QStringLiteral("error"), QStringLiteral("binary file not readable")}};
    }
    QByteArray remaining = f.readAll();
    f.close();
    QByteArray full = sniff + remaining;
    bool truncated = false;
    if (full.size() > kLlmReturnCap) {
        full.truncate(kLlmReturnCap);
        truncated = true;
    }
    QString text = QString::fromUtf8(full);
    if (truncated) {
        text += QStringLiteral("\n[truncated: %1+ remaining]")
                    .arg(remaining.size() + sniff.size() - kLlmReturnCap);
    }

    QJsonObject out;
    out.insert(QStringLiteral("skill_id"), skillId);
    out.insert(QStringLiteral("relative_path"), rel);
    out.insert(QStringLiteral("content"), text);
    out.insert(QStringLiteral("truncated"), truncated);
    return out;
}

}  // namespace Tools
