// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file heartbeat-report-parser.h
 * @brief Pure parser for the structured report produced by a Tier-1
 *        subagent run.
 *
 *        Subagents are instructed (via the system-prompt addendum in
 *        RequestBuilder::buildSubagentRequest) to emit:
 *
 *          TITLE: <one-line summary, max 100 chars>
 *          RESULTS:
 *          <multi-line body>
 *          SUMMARY:
 *          <≤ 3 sentences>
 *
 *        This parser splits the model's textual output into the three
 *        named fields. All three labels are case-insensitive and the
 *        whitespace after them is flexible (trailing newline, space,
 *        or end-of-line) to be tolerant of model output drift.
 *
 *        The parser is total: never throws. On malformed input the
 *        valid flag is false and the raw output is returned in `body`
 *        with empty title/summary so the caller can still persist the
 *        run with diagnostics rather than losing data.
 *
 * @layer Utility (pure function)
 * @dependencies Qt6::Core
 */


#pragma once

#include <QString>

/**
 * @brief Parsed shape of the subagent's structured output.
 */
struct ParsedHeartbeatReport {
    QString title;  ///< TITLE: line content, truncated to 100 characters.
    /// RESULTS: section content. When valid is false, holds the whole
    /// raw output instead, so the report row still records what the
    /// model said.
    QString body;
    QString summary;     ///< SUMMARY: section content.
    bool valid = false;  ///< True when RESULTS or SUMMARY was found.
};

/**
 * @brief Parse a subagent's structured TITLE / RESULTS / SUMMARY output.
 * @param raw The full textual output captured from the LLM.
 * @return ParsedHeartbeatReport. Always returns; never throws.
 *         On malformed input `valid` is false and `body` carries the
 *         original raw text so the caller can persist the run for
 *         diagnostics.
 *
 * @complexity O(N) in input length. No regex backtracking; simple
 *             label-scan + section-split.
 */
ParsedHeartbeatReport parseHeartbeatReport(const QString& raw);
