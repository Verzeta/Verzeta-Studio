// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file tool-payload-digest.h
 * @brief Pure, idempotent compaction of historical tool-call arguments and
 *        tool results for the LLM context window.
 * @layer Service (Chat subsystem)
 * @dependencies Qt6::Core (QJsonObject / QString only).
 *
 * Why this exists: every write_file / open_canvas / edit_canvas call embeds
 * the FULL file body in its arguments, and search_web / read_* results carry
 * their full payload, all replayed verbatim in history on every turn, forever
 * (a live conversation accumulated 35 KB of tool-call args + 15 KB of results,
 * filling the window and starving the model's reply to ~160 tokens). These
 * functions produce a faithful, compact REFERENCE to drop into the LLM's view
 * of HISTORY while the full content stays in the database (transcript / export
 * / search_messages are untouched) and remains re-fetchable by the model via
 * read_file / read_canvas / search_messages, the designed escape hatch.
 *
 * Pure: no DB, no services, no Qt-GUI; deterministic; idempotent (digesting an
 * already-digested payload is a no-op). The DECISION of which payloads to
 * digest (token-share cap, per-payload ceiling, protected last group) lives in
 * RequestBuilder::assembleHistory; this class only performs the compaction.
 */
#pragma once

#include <QJsonObject>
#include <QString>

namespace Chat {

/**
 * @brief Stateless compactor for tool-call arguments and tool results.
 */
class ToolPayloadDigest {
  public:
    /// Sentinel key inserted into a digested arguments object. Its presence
    /// marks the object as already digested (idempotence guard).
    static constexpr const char* kArgsDigestKey = "__digest";

    /// Sentinel prefix on every digested result string. Doubles as a hint to
    /// the model that this is a compacted reference, and is the idempotence
    /// guard (a string already starting with it is returned unchanged).
    static constexpr const char* kResultDigestPrefix = "[digest] ";

    /// String argument/result fields longer than this are elided by the
    /// generic path; shorter scalars (filename, mode, query) are kept verbatim.
    static constexpr int kFieldElideThreshold = 160;

    /// Leading-snippet length kept from a digested read-tool result body.
    /// A read result is content the model fetched to USE. Digesting it to a
    /// bare "re-read" instruction is circular, so a digested (old) read keeps
    /// the head of the actual content instead.
    static constexpr int kReadSnippetChars = 400;

    /**
     * @brief Compact a historical tool call's arguments.
     * @param toolName The tool the arguments belong to (drives the summary
     *                 shape: write_file / open_canvas / edit_canvas get a
     *                 "<file> · N lines · KB" summary with the body stripped;
     *                 every other tool keeps its small fields and elides any
     *                 oversized string field).
     * @param args     The full arguments object.
     * @returns A new object: small fields kept verbatim, the body/large
     *          fields replaced by a short marker, plus a kArgsDigestKey
     *          summary. Idempotent: an object that already contains
     *          kArgsDigestKey is returned unchanged.
     */
    static QJsonObject digestArgs(const QString& toolName, const QJsonObject& args);

    /**
     * @brief Compact a historical tool result body.
     * @param toolName   The tool that produced the result.
     * @param resultJson The full result text (JSON or plain) as persisted.
     * @returns A compact reference string: search_web keeps the answer + a
     *          few source titles/urls; read_* / write_file keep a one-line
     *          "[read/wrote <path> …]"; every other tool elides an oversized
     *          body to a bracketed "[\<tool\> result · N chars elided ...]"
     *          marker that ends with a "re-run to re-fetch" hint.
     *          Idempotent: an already-digested string is returned unchanged.
     */
    static QString digestResult(const QString& toolName, const QString& resultJson);

    /**
     * @brief One-line prose summary of a whole historical tool call + result,
     *        for the collapse-to-prose digest representation.
     *
     * Unlike digestArgs (which returns a structured arguments object), this
     * returns plain text, e.g. "write_file index.html (49 lines, 1602 B) →
     * wrote index.html". It is rendered into a role=system record that REPLACES
     * the structured assistant{tool_calls} + role=tool rows in the model's view
     * of OLD history, so the model can never copy a structurally-valid but
     * body-less tool call (the `__digest` arg-leak that made the model emit
     * write_file with no content). The full content stays in the DB and is
     * re-fetchable via read_file / read_canvas / search_messages.
     *
     * @param toolName   The tool that was called.
     * @param args       The original full arguments object.
     * @param resultJson The original full result text (JSON or plain).
     * @returns A single-line, human-readable record (no newlines).
     */
    static QString
    summarizeCall(const QString& toolName, const QJsonObject& args, const QString& resultJson);

    /**
     * @brief Whether an arguments object has already been digested.
     * @param args The arguments object to test.
     * @returns true iff @p args contains the digest sentinel key.
     */
    static bool isDigestedArgs(const QJsonObject& args);

    /**
     * @brief Whether a result string has already been digested.
     * @param result The result string to test.
     * @returns true iff @p result starts with the digest sentinel prefix.
     */
    static bool isDigestedResult(const QString& result);
};

}  // namespace Chat
