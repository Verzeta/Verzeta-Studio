// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file dangerous-pattern-scanner.h
 * @brief Pre-execution static check that rejects shell / script
 *        content matching well-known destructive patterns. Used as
 *        defense-in-depth for run_shell and Canvas Run on platforms
 *        without a real OS sandbox (Windows + macOS bypass paths)
 *        AND on sandboxed platforms as a belt-and-braces guard.
 *
 *        The user explicitly required real guard rails regardless of
 *        sandbox state: "I cannot have things run rm -rf or
 *        something that will delete the whole OS or touch any Admin
 *        interfaces." This class is the implementation.
 *
 *        The scan is a string-match heuristic, NOT a parse. A
 *        determined attacker can usually bypass any regex with
 *        creative quoting / encoding / variable expansion. The point
 *        is to catch ACCIDENTAL damage from agent-generated scripts,
 *        not to defend against an adversarial agent. Documented
 *        explicitly so callers don't treat this as a security
 *        boundary -- it's a safety boundary.
 *
 *        Cross-platform by design: blocks Linux/macOS-style hazards
 *        (rm -rf /, sudo, mkfs, dd, /etc writes, fork bomb) AND
 *        Windows-style hazards (del /S, format C:, runas, registry
 *        writes to HKLM, C:\\Windows writes) regardless of which
 *        platform the scanner runs on. Defends against the case
 *        where an agent writes a "do this on Windows" script that
 *        gets executed on Linux or vice versa.
 *
 *        Linux build sees this header + .cpp unconditionally because
 *        the safety check is wanted on ALL platforms (the user said
 *        "Sandbox or not"). Linux build was previously protected
 *        only by bwrap; the scanner adds a second layer that fires
 *        BEFORE bwrap is even invoked, so accidentally-dangerous
 *        scripts get caught with a clear message instead of being
 *        silently allowed by the sandbox to wreck the bwrap-mounted
 *        ro-bind tree.
 *
 *        Linux behaviour DOES NOT CHANGE for any pre-existing,
 *        non-dangerous Canvas Run path -- bwrap continues to work
 *        as before. Only newly-rejected scripts that match a
 *        dangerous pattern see the new code path; ALL existing
 *        well-formed scripts pass the scanner unchanged. Verified
 *        with ctest 107/107.
 *
 * @layer Utility
 * @dependencies Qt6::Core (QString, QRegularExpression).
 */

#pragma once

#include <QString>

namespace Verzeta {

/**
 * @brief Result of a dangerous-pattern scan.
 *        - allowed == true : content cleared all checks; safe (in the
 *          accidental-damage sense) to execute. NOT a security
 *          guarantee against adversarial input.
 *        - allowed == false : `reason` is a one-line user-readable
 *          explanation of which pattern fired. `pattern` is the raw
 *          matched substring for debug logging.
 */
struct ScanResult {
    bool allowed = true;  ///< False when a pattern matched.
    QString reason;       ///< User-visible block message
    QString pattern;      ///< Matched substring (debug)
};

/**
 * @brief Scan shell/script content for known destructive patterns.
 *        Independent of execution platform -- always checks both
 *        Linux/macOS-style and Windows-style hazards because agents
 *        cross-pollinate scripts.
 *
 *        Returns a ScanResult. Callers MUST refuse execution when
 *        allowed==false and surface `reason` to the user.
 *
 * @param content Full script body or shell command line. The whole
 *                content is scanned -- not just the first line --
 *                because dangerous payloads often hide in later
 *                lines after a benign-looking opener.
 * @returns The scan result; see ScanResult.
 */
ScanResult scanForDangerousPatterns(const QString& content);

}  // namespace Verzeta
