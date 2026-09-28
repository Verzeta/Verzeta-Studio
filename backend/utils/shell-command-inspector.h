// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file shell-command-inspector.h
 * @brief Detects shell command forms that run code which cannot be checked
 *        before it runs: eval of text built from variables, a shell whose
 *        -c script is built from variables, and text piped into a shell.
 *        Everything else passes unchanged; the program allow-list and the
 *        destructive-pattern scanner stay as they are.
 * @layer Utility
 * @dependencies Qt6::Core
 */

#pragma once

#include <QString>
#include <QStringList>

namespace Verzeta {

/**
 * @brief Result of inspecting one command line.
 */
struct ShellInspection {
    /** @brief Program names (basenames) found in command position, including
     *         those inside `$(...)`, backticks and `bash -c '...'`. Shell
     *         keywords, harmless builtins and wrappers such as `env` or
     *         `timeout` are not listed. Informational. */
    QStringList programs;

    /** @brief Non-empty when the command runs code that cannot be checked
     *         beforehand; the caller refuses the command and shows this. */
    QString refusal;

    /** @brief False when the line could not be fully parsed (for example an
     *         unterminated quote). Such lines are NOT refused here: the
     *         shell reports its own syntax error. */
    bool complete = true;
};

/**
 * @brief Inspects a command line the way bash would split it.
 *
 * Understands `;`, `&&`, `||`, `|`, `&`, newlines, subshells, redirections,
 * `NAME=value` prefixes, heredocs (quoted bodies are data; unquoted bodies
 * are scanned for command substitutions), command and process
 * substitution, and `sh|bash|zsh|dash -c`, whose literal script is
 * inspected recursively. Only three forms are refused: `eval` of text
 * built from variables or substitutions, a shell `-c` script built from
 * variables or substitutions, and a shell reading its script from a pipe.
 *
 * @param command Command line as passed to `bash -c`.
 * @returns The programs found and, for the three refused forms, the reason.
 * @complexity O(n) in the command length per nesting level.
 */
ShellInspection inspectPosixCommand(const QString& command);

/**
 * @brief Whether a word is a shell builtin that runs no other program
 *        (cd, export, echo, test, true, ...).
 * @param name Command name.
 * @returns true for the fixed set of harmless builtins.
 */
bool isHarmlessShellBuiltin(const QString& name);

}  // namespace Verzeta
