// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file skill-parser.h
 * @brief Pure parser for a candidate skill folder.  Validates
 *        structure (SKILL.md present, ID grammar, file size caps,
 *        no symlinks, no path traversal in frontmatter), parses
 *        YAML-ish frontmatter, scans every text file for the
 *        suspicious-marker regex set, and emits a Skill POD plus
 *        the warnings list.
 *
 *        The parser does NOT touch the filesystem outside the input
 *        folder, does NOT write manifests, and does NOT execute
 *        anything inside the skill. It is callable from tests and
 *        from SkillService alike.
 * @layer Utility
 * @dependencies Qt6::Core.  Pure parser, no service deps.
 */

#pragma once

#include "../models/skill.h"

#include <QString>

namespace SkillParser {

/**
 * @brief Parse outcome.
 *
 *        On success: `success=true`, `skill` populated (without
 *        `installPath` / `installedAtMs`, which SkillService fills),
 *        `warnings` populated (may be empty).
 *
 *        On failure: `success=false`, `error` populated with a
 *        user-readable reason. Caller (SkillService) moves the input
 *        into `skills/quarantine/<ts>-\<id\>/`.
 */
struct ParseResult {
    bool success = false;          ///< True when the skill parsed.
    QString error;                 ///< User-readable reason on failure.
    Skill skill;                   ///< Parsed skill on success.
    QList<SkillWarning> warnings;  ///< Scanner findings; may be empty.
};

/**
 * @brief Validate a skill folder + parse its SKILL.md frontmatter +
 *        scan all text files for suspicious markers.
 *
 * @param folderPath  Absolute path to the candidate skill folder.
 *                    Must exist; SKILL.md must be present at the root.
 * @param idHint      Optional id to use when frontmatter has no
 *                    `name`. Falls back to the folder basename when
 *                    empty. ClawHub installs pass the slug here so a
 *                    flat-ZIP layout (no top-level dir wrapper) still
 *                    produces a valid id rather than leaking the
 *                    `.staging-\<uuid\>` directory name.
 * @return ParseResult (see struct doc).
 * @complexity O(F + sum(text file sizes)) for file walk and regex
 *             scan. Hash computation is delegated to SkillHash and
 *             called by SkillService AFTER parser approval.
 * @sideeffects None (pure read).
 */
ParseResult parseSkillFolder(const QString& folderPath, const QString& idHint = {});

/**
 * @brief Returns true iff the input matches the skill-id grammar:
 *        `^[a-z0-9][a-z0-9_-]{0,63}$`. Used by tests + SkillService
 *        UI validation.
 * @param candidate Proposed skill id.
 * @returns true when candidate matches the grammar.
 */
bool isValidSkillId(const QString& candidate);

}  // namespace SkillParser
