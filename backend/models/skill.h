// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file skill.h
 * @brief Skill PODs.
 *
 *        Skills are file-backed instruction bundles that agents can call
 *        upon to perform automation workflows using the existing tool
 *        surface. They never grant tools and never execute scripts.
 * @layer Model
 * @dependencies Qt6::Core. Pure value types, no service or DB deps.
 */

#pragma once

#include <QDateTime>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

/**
 * @brief One suspicious-marker hit recorded by SkillParser.
 *
 *        Surfaced in the Review Dialog so the user sees concrete
 *        evidence of why the parser flagged a skill. Warnings do NOT
 *        auto-block; they keep the skill `unreviewed` until the user
 *        explicitly approves with the responsibility checkbox.
 */
struct SkillWarning {
    /** One of the named regex constants in SkillParser (e.g.
     *  WARN_PIPE_TO_SHELL). Stable across versions for log filtering. */
    QString regexName;
    /** Path of the file inside the skill folder where the match landed,
     *  relative to the skill root (POSIX form, no leading `/`). */
    QString fileRelativePath;
    /** 1-indexed line number inside the file. */
    int lineNumber = 0;
    /** Up to 120 chars of the matched substring, for human review. */
    QString matchedExcerpt;

    /**
     * @brief Serialises this warning to its JSON form on disk.
     * @returns JSON object with the four warning fields.
     */
    QJsonObject toJson() const {
        QJsonObject o;
        o.insert(QStringLiteral("regex_name"), regexName);
        o.insert(QStringLiteral("file_relative_path"), fileRelativePath);
        o.insert(QStringLiteral("line_number"), lineNumber);
        o.insert(QStringLiteral("matched_excerpt"), matchedExcerpt);
        return o;
    }

    /**
     * @brief Deserialises a warning from its on-disk JSON form.
     * @param o  JSON object previously produced by toJson().
     * @returns Populated SkillWarning. Missing fields fall back to
     *          default-constructed values.
     */
    static SkillWarning fromJson(const QJsonObject& o) {
        SkillWarning w;
        w.regexName = o.value(QStringLiteral("regex_name")).toString();
        w.fileRelativePath = o.value(QStringLiteral("file_relative_path")).toString();
        w.lineNumber = o.value(QStringLiteral("line_number")).toInt();
        w.matchedExcerpt = o.value(QStringLiteral("matched_excerpt")).toString();
        return w;
    }
};

/**
 * @brief Full installed-skills.json row + parser-derived metadata.
 *
 *        `reviewState` is stored in review-state.json (separate file)
 *        but populated here at SkillService load time so callers can
 *        filter without a second lookup.
 */
struct Skill {
    QString id;                 ///< Skill-id grammar
    QString source;             ///< "manual" | "clawhub"
    QString sourceUrl;          ///< optional, for clawhub
    QString version;            ///< semver if provided; opaque otherwise
    QString installPath;        ///< relative to AppData ("skills/installed/<id>")
    QString displayName;        ///< human-readable frontmatter `name`; falls back to id
    QString description;        ///< from frontmatter
    QStringList tags;           ///< from frontmatter
    QStringList declaredTools;  ///< from frontmatter, advisory only
    qint64 installedAtMs = 0;   ///< First install time, ms since epoch.
    qint64 updatedAtMs = 0;     ///< Last install or update time, ms since epoch.
    /// SHA-256 of the installed skill folder. Review approval is bound
    /// to this value.
    QString contentHashSha256;
    QList<SkillWarning> warnings;  ///< Scanner findings from install time.
    /** Populated from review-state.json: "unreviewed" / "approved" /
     *  "blocked" / "quarantined". */
    QString reviewState = QStringLiteral("unreviewed");

    /**
     * @brief Reports whether the row carries a non-empty skill id.
     * @returns True iff `id` is non-empty.
     */
    bool isValid() const { return !id.isEmpty(); }

    /**
     * @brief Returns whether the user has approved this skill for use.
     * @returns True iff `reviewState` is "approved".
     */
    bool isApproved() const { return reviewState == QStringLiteral("approved"); }

    /**
     * @brief Returns whether the user has blocked this skill.
     * @returns True iff `reviewState` is "blocked".
     */
    bool isBlocked() const { return reviewState == QStringLiteral("blocked"); }
};

/**
 * @brief Compact summary injected into the AVAILABLE SKILLS prompt
 *        layer + returned by `discover_skills`.
 *
 *        Token-budgeted in RequestBuilder; full Skill payload (warnings,
 *        hash, paths) stays out of the prompt.
 */
struct SkillSummary {
    QString id;                 ///< Skill id.
    QString description;        ///< Frontmatter description.
    QStringList tags;           ///< Frontmatter tags.
    QStringList declaredTools;  ///< Tools the skill says it uses; advisory.
    QString version;            ///< Skill version, if declared.

    /**
     * @brief Builds a SkillSummary from a full Skill record.
     * @param s  Source skill.
     * @returns Compact summary containing only the prompt-facing fields.
     */
    static SkillSummary fromSkill(const Skill& s) {
        SkillSummary out;
        out.id = s.id;
        out.description = s.description;
        out.tags = s.tags;
        out.declaredTools = s.declaredTools;
        out.version = s.version;
        return out;
    }
};

/**
 * @brief One review-state.json row. State binds approval to the exact
 *        (id, version, hash) tuple. Any byte change in the skill
 *        folder bumps the hash, which resets state to `unreviewed`.
 */
struct SkillReviewDecision {
    QString skillId;            ///< Skill the decision applies to.
    QString version;            ///< Skill version the decision was made on.
    QString contentHashSha256;  ///< Folder hash the decision was made on.
    QString state;              ///< "unreviewed" | "approved" | "blocked" | "quarantined"
    qint64 decidedAtMs = 0;     ///< Decision time, ms since epoch.
    QList<SkillWarning> warningsAtDecisionTime;  ///< Warnings the user saw when deciding.

    /**
     * @brief Serialises this decision to its on-disk JSON form.
     * @returns JSON object containing the decision tuple and the
     *          warnings the user reviewed.
     */
    QJsonObject toJson() const {
        QJsonObject o;
        o.insert(QStringLiteral("skill_id"), skillId);
        o.insert(QStringLiteral("version"), version);
        o.insert(QStringLiteral("content_hash_sha256"), contentHashSha256);
        o.insert(QStringLiteral("state"), state);
        o.insert(QStringLiteral("decided_at_ms"),
                 QJsonValue::fromVariant(QVariant::fromValue(decidedAtMs)));
        QJsonArray warr;
        for (const SkillWarning& w : warningsAtDecisionTime) {
            warr.append(w.toJson());
        }
        o.insert(QStringLiteral("warnings_at_decision_time"), warr);
        return o;
    }

    /**
     * @brief Deserialises a review decision from its on-disk JSON form.
     * @param o  JSON object previously produced by toJson().
     * @returns Populated SkillReviewDecision; missing fields fall back
     *          to default-constructed values.
     */
    static SkillReviewDecision fromJson(const QJsonObject& o) {
        SkillReviewDecision d;
        d.skillId = o.value(QStringLiteral("skill_id")).toString();
        d.version = o.value(QStringLiteral("version")).toString();
        d.contentHashSha256 = o.value(QStringLiteral("content_hash_sha256")).toString();
        d.state = o.value(QStringLiteral("state")).toString();
        d.decidedAtMs = static_cast<qint64>(o.value(QStringLiteral("decided_at_ms")).toDouble());
        const QJsonArray warr = o.value(QStringLiteral("warnings_at_decision_time")).toArray();
        for (const QJsonValue& v : warr) {
            d.warningsAtDecisionTime.append(SkillWarning::fromJson(v.toObject()));
        }
        return d;
    }
};

/**
 * @brief One preferred-skills.json row: pure references into the
 *        installed library, scoped by (scope_type, scope_id).
 *
 *        `overrideParentFolder` is meaningful only for conversations
 *        whose folder chain has a project / organization with a
 *        non-empty preferred list. Folder rows ignore it.
 */
struct PreferredSkillsScope {
    QString scopeType;              ///< "conversation_1to1" | "conversation_group" | "folder"
    QString scopeId;                ///< conversation or folder UUID
    QStringList preferredSkillIds;  ///< user-ordered
    /// When true, the preferred skills are the only ones offered in this
    /// scope; when false, other approved skills stay discoverable.
    bool exposeOnlyPreferred = false;
    /// Conversation rows only. When true and this row's list is not
    /// empty, it wins over the parent folder's list; otherwise a
    /// non-empty folder list wins.
    bool overrideParentFolder = false;
    qint64 updatedAtMs = 0;  ///< Last change, ms since epoch.

    /**
     * @brief Serialises this scope to its on-disk JSON form.
     * @returns JSON object describing the scope and its preferred list.
     */
    QJsonObject toJson() const {
        QJsonObject o;
        o.insert(QStringLiteral("scope_type"), scopeType);
        o.insert(QStringLiteral("scope_id"), scopeId);
        QJsonArray arr;
        for (const QString& id : preferredSkillIds)
            arr.append(id);
        o.insert(QStringLiteral("preferred_skill_ids"), arr);
        o.insert(QStringLiteral("expose_only_preferred"), exposeOnlyPreferred);
        o.insert(QStringLiteral("override_parent_folder"), overrideParentFolder);
        o.insert(QStringLiteral("updated_at_ms"),
                 QJsonValue::fromVariant(QVariant::fromValue(updatedAtMs)));
        return o;
    }

    /**
     * @brief Deserialises a preferred-skills scope from its on-disk JSON form.
     * @param o  JSON object previously produced by toJson().
     * @returns Populated PreferredSkillsScope; missing fields fall back
     *          to default-constructed values.
     */
    static PreferredSkillsScope fromJson(const QJsonObject& o) {
        PreferredSkillsScope p;
        p.scopeType = o.value(QStringLiteral("scope_type")).toString();
        p.scopeId = o.value(QStringLiteral("scope_id")).toString();
        const QJsonArray arr = o.value(QStringLiteral("preferred_skill_ids")).toArray();
        for (const QJsonValue& v : arr)
            p.preferredSkillIds.append(v.toString());
        p.exposeOnlyPreferred = o.value(QStringLiteral("expose_only_preferred")).toBool();
        p.overrideParentFolder = o.value(QStringLiteral("override_parent_folder")).toBool();
        p.updatedAtMs = static_cast<qint64>(o.value(QStringLiteral("updated_at_ms")).toDouble());
        return p;
    }
};
