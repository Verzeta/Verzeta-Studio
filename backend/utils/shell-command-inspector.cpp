// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later

/**
 * @file shell-command-inspector.cpp
 * @brief Implementation of the POSIX command inspector: a quote-aware lexer
 *        that follows substitutions and heredocs, and a small parser that
 *        finds the word in command position of every simple command and
 *        refuses only code that cannot be checked before it runs.
 * @layer Utility
 * @dependencies Qt6::Core
 */

#include "shell-command-inspector.h"

#include <QRegularExpression>
#include <QSet>
#include <QVector>

namespace Verzeta {

namespace {

// Nesting limit for $(...), backticks, bash -c and eval. Deeper input is
// refused rather than inspected partially.
constexpr int kMaxDepth = 16;

/**
 * @brief One lexical token: a word (quotes removed), an operator or a
 *        redirection.
 */
struct Token {
    enum Kind { Word, Op, Redirect };
    Kind kind = Word;
    QString text;          // Word: value with quotes removed; Op/Redirect: operator
    bool dynamic = false;  // Word contains a $ expansion or substitution
};

/**
 * @brief Quote-aware lexer for one command line. Follows command and
 *        process substitutions and heredocs, inspecting nested command text
 *        recursively into the shared result.
 */
class Lexer {
  public:
    Lexer(const QString& s, int depth, ShellInspection& out) : m_s(s), m_depth(depth), m_out(out) {}

    /**
     * @brief Splits the command line into tokens.
     * @returns The tokens up to the point the lexer could follow; parse
     *          problems mark the result incomplete instead of refusing.
     */
    QVector<Token> run() {
        QVector<Token> toks;
        while (m_i < m_s.size() && !m_stop) {
            const QChar c = m_s.at(m_i);
            if (c == QLatin1Char(' ') || c == QLatin1Char('\t')) {
                ++m_i;
                continue;
            }
            if (c == QLatin1Char('\\') && peek(1) == QLatin1Char('\n')) {
                m_i += 2;
                continue;
            }
            if (c == QLatin1Char('#')) {  // comment to end of line
                while (m_i < m_s.size() && m_s.at(m_i) != QLatin1Char('\n'))
                    ++m_i;
                continue;
            }
            if (c == QLatin1Char('\n')) {
                ++m_i;
                toks.append({Token::Op, QStringLiteral("\n"), false});
                consumeHeredocs();
                continue;
            }
            if (Token t; readOperator(t)) {
                toks.append(t);
                continue;
            }
            toks.append(readWord());
        }
        if (!m_stop && !m_heredocs.isEmpty())
            consumeHeredocs();
        return toks;
    }

  private:
    /** @brief A heredoc waiting for its body after the current line. */
    struct Heredoc {
        QString delim;
        bool quoted;
        bool stripTabs;
    };

    QChar peek(int off) const {
        const int j = m_i + off;
        return (j >= 0 && j < m_s.size()) ? m_s.at(j) : QChar();
    }

    // A line the lexer cannot follow is left to the shell, which reports
    // its own syntax error; inspection just stops.
    void fail(const QString& why) {
        Q_UNUSED(why);
        m_out.complete = false;
        m_stop = true;
    }

    void recurse(const QString& inner) {
        if (m_depth + 1 > kMaxDepth) {
            fail(QStringLiteral("it is nested too deeply"));
            return;
        }
        ShellInspection sub = inspectPosixCommandAt(inner, m_depth + 1);
        m_out.programs.append(sub.programs);
        if (!sub.complete)
            m_out.complete = false;
        if (!sub.refusal.isEmpty() && m_out.refusal.isEmpty())
            m_out.refusal = sub.refusal;
    }

    bool readOperator(Token& t) {
        static const char* ops[] = {"&&", "||", ";;", "|&", ";", "|", "&", "(", ")"};
        // Redirections first (an optional fd number, then the operator).
        int j = m_i;
        while (j < m_s.size() && m_s.at(j).isDigit())
            ++j;
        static const char* redirs[] = {
            "<<<", "<<-", "&>>", "<<", ">>", ">&", "<&", "&>", ">|", "<>", ">", "<"};
        for (const char* r : redirs) {
            const QString rs = QString::fromLatin1(r);
            if (m_s.mid(j, rs.size()) == rs) {
                // "&>" only counts as a redirect when not part of "&&".
                if (rs.startsWith(QLatin1Char('&')) && j != m_i)
                    break;
                m_i = j + rs.size();
                t = {Token::Redirect, rs, false};
                if (rs == QLatin1String("<<") || rs == QLatin1String("<<-"))
                    readHeredocDelimiter(rs == QLatin1String("<<-"));
                return true;
            }
        }
        if (j != m_i)
            return false;  // digits that are not an fd: part of a word
        for (const char* o : ops) {
            const QString os = QString::fromLatin1(o);
            if (m_s.mid(m_i, os.size()) == os) {
                // "(" directly after "<" or ">" is process substitution,
                // handled in readWord; here it is a subshell.
                m_i += os.size();
                t = {Token::Op, os, false};
                return true;
            }
        }
        return false;
    }

    void readHeredocDelimiter(bool stripTabs) {
        while (m_i < m_s.size() &&
               (m_s.at(m_i) == QLatin1Char(' ') || m_s.at(m_i) == QLatin1Char('\t')))
            ++m_i;
        const int start = m_i;
        const Token w = readWord();
        const QString raw = m_s.mid(start, m_i - start);
        const bool quoted = raw.contains(QLatin1Char('\'')) || raw.contains(QLatin1Char('"')) ||
                            raw.contains(QLatin1Char('\\'));
        if (w.text.isEmpty()) {
            fail(QStringLiteral("a heredoc has no delimiter"));
            return;
        }
        m_heredocs.append({w.text, quoted, stripTabs});
    }

    void consumeHeredocs() {
        for (const Heredoc& h : std::as_const(m_heredocs)) {
            QString body;
            bool closed = false;
            while (m_i < m_s.size()) {
                int eol = m_s.indexOf(QLatin1Char('\n'), m_i);
                if (eol < 0)
                    eol = m_s.size();
                QString line = m_s.mid(m_i, eol - m_i);
                m_i = qMin(eol + 1, int(m_s.size()));
                QString cmp = line;
                if (h.stripTabs)
                    while (cmp.startsWith(QLatin1Char('\t')))
                        cmp.remove(0, 1);
                if (cmp == h.delim) {
                    closed = true;
                    break;
                }
                body += line + QLatin1Char('\n');
            }
            if (!closed) {
                fail(QStringLiteral("a heredoc is not terminated"));
                return;
            }
            // An unquoted heredoc body is expanded by the shell, so any
            // $(...) or backticks in it run. Check those, nothing else.
            if (!h.quoted)
                scanExpansions(body);
        }
        m_heredocs.clear();
    }

    // Finds $(...) and backtick substitutions in text that is otherwise data.
    void scanExpansions(const QString& text) {
        for (int k = 0; k < text.size() && !m_stop; ++k) {
            const QChar c = text.at(k);
            if (c == QLatin1Char('\\')) {
                ++k;
                continue;
            }
            if (c == QLatin1Char('$') && k + 1 < text.size() &&
                text.at(k + 1) == QLatin1Char('(')) {
                if (k + 2 < text.size() && text.at(k + 2) == QLatin1Char('(')) {
                    const int end = text.indexOf(QStringLiteral("))"), k + 3);
                    if (end < 0) {
                        fail(QStringLiteral("arithmetic is not closed"));
                        return;
                    }
                    k = end + 1;
                    continue;
                }
                const int end = matchParen(text, k + 1);
                if (end < 0) {
                    fail(QStringLiteral("a $( is not closed"));
                    return;
                }
                recurse(text.mid(k + 2, end - k - 2));
                k = end;
            } else if (c == QLatin1Char('`')) {
                const int end = text.indexOf(QLatin1Char('`'), k + 1);
                if (end < 0) {
                    fail(QStringLiteral("a backtick is not closed"));
                    return;
                }
                recurse(text.mid(k + 1, end - k - 1));
                k = end;
            }
        }
    }

    // Index of the ')' closing the '(' at openIdx, honouring quotes and
    // nesting; -1 if unbalanced.
    static int matchParen(const QString& s, int openIdx) {
        int depth = 0;
        for (int k = openIdx; k < s.size(); ++k) {
            const QChar c = s.at(k);
            if (c == QLatin1Char('\\')) {
                ++k;
                continue;
            }
            if (c == QLatin1Char('\'')) {
                const int e = s.indexOf(QLatin1Char('\''), k + 1);
                if (e < 0)
                    return -1;
                k = e;
                continue;
            }
            if (c == QLatin1Char('"')) {
                for (++k; k < s.size() && s.at(k) != QLatin1Char('"'); ++k)
                    if (s.at(k) == QLatin1Char('\\'))
                        ++k;
                if (k >= s.size())
                    return -1;
                continue;
            }
            if (c == QLatin1Char('('))
                ++depth;
            else if (c == QLatin1Char(')') && --depth == 0)
                return k;
        }
        return -1;
    }

    // Reads a $-expansion starting at m_i ('$'); appends its source text.
    void readDollar(QString& value, bool& dynamic) {
        dynamic = true;
        if (peek(1) == QLatin1Char('(')) {
            if (peek(2) == QLatin1Char('(')) {
                const int end = m_s.indexOf(QStringLiteral("))"), m_i + 3);
                if (end < 0) {
                    fail(QStringLiteral("arithmetic is not closed"));
                    m_i = m_s.size();
                    return;
                }
                value += m_s.mid(m_i, end + 2 - m_i);
                m_i = end + 2;
                return;
            }
            const int end = matchParen(m_s, m_i + 1);
            if (end < 0) {
                fail(QStringLiteral("a $( is not closed"));
                m_i = m_s.size();
                return;
            }
            recurse(m_s.mid(m_i + 2, end - m_i - 2));
            value += m_s.mid(m_i, end + 1 - m_i);
            m_i = end + 1;
            return;
        }
        if (peek(1) == QLatin1Char('{')) {
            const int end = m_s.indexOf(QLatin1Char('}'), m_i + 2);
            if (end < 0) {
                fail(QStringLiteral("a ${ is not closed"));
                m_i = m_s.size();
                return;
            }
            value += m_s.mid(m_i, end + 1 - m_i);
            m_i = end + 1;
            return;
        }
        if (peek(1) == QLatin1Char('\'')) {  // $'...' ANSI-C quoting: literal
            dynamic = false;
            const int end = m_s.indexOf(QLatin1Char('\''), m_i + 2);
            if (end < 0) {
                fail(QStringLiteral("a quote is not closed"));
                m_i = m_s.size();
                return;
            }
            value += m_s.mid(m_i + 2, end - m_i - 2);
            m_i = end + 1;
            return;
        }
        value += QLatin1Char('$');
        ++m_i;
        while (m_i < m_s.size() &&
               (m_s.at(m_i).isLetterOrNumber() || m_s.at(m_i) == QLatin1Char('_') ||
                QStringLiteral("@*#?$!-").contains(m_s.at(m_i)))) {
            value += m_s.at(m_i++);
            if (!value.back().isLetterOrNumber() && value.back() != QLatin1Char('_'))
                break;
        }
    }

    Token readWord() {
        Token t;
        QString& value = t.text;
        static const QString kBreak = QStringLiteral(" \t\n;&|()<>");
        while (m_i < m_s.size() && !m_stop) {
            const QChar c = m_s.at(m_i);
            if (kBreak.contains(c)) {
                // <(...) and >(...) at word start: process substitution.
                if ((c == QLatin1Char('<') || c == QLatin1Char('>')) && value.isEmpty() &&
                    peek(1) == QLatin1Char('(')) {
                    const int end = matchParen(m_s, m_i + 1);
                    if (end < 0) {
                        fail(QStringLiteral("a process substitution is not closed"));
                        break;
                    }
                    recurse(m_s.mid(m_i + 2, end - m_i - 2));
                    t.dynamic = true;
                    m_i = end + 1;
                    continue;
                }
                break;
            }
            if (c == QLatin1Char('\\')) {
                if (m_i + 1 < m_s.size())
                    value += m_s.at(m_i + 1);
                m_i += 2;
                continue;
            }
            if (c == QLatin1Char('\'')) {
                const int end = m_s.indexOf(QLatin1Char('\''), m_i + 1);
                if (end < 0) {
                    fail(QStringLiteral("a single quote is not closed"));
                    m_i = m_s.size();
                    break;
                }
                value += m_s.mid(m_i + 1, end - m_i - 1);
                m_i = end + 1;
                continue;
            }
            if (c == QLatin1Char('"')) {
                ++m_i;
                bool closed = false;
                while (m_i < m_s.size() && !m_stop) {
                    const QChar d = m_s.at(m_i);
                    if (d == QLatin1Char('"')) {
                        ++m_i;
                        closed = true;
                        break;
                    }
                    if (d == QLatin1Char('\\') && m_i + 1 < m_s.size()) {
                        value += m_s.at(m_i + 1);
                        m_i += 2;
                        continue;
                    }
                    if (d == QLatin1Char('$')) {
                        bool dyn = false;
                        readDollar(value, dyn);
                        t.dynamic = t.dynamic || dyn;
                        continue;
                    }
                    if (d == QLatin1Char('`')) {
                        readBacktick(value);
                        t.dynamic = true;
                        continue;
                    }
                    value += d;
                    ++m_i;
                }
                if (!closed) {
                    fail(QStringLiteral("a double quote is not closed"));
                    break;
                }
                continue;
            }
            if (c == QLatin1Char('$')) {
                bool dyn = false;
                readDollar(value, dyn);
                t.dynamic = t.dynamic || dyn;
                continue;
            }
            if (c == QLatin1Char('`')) {
                readBacktick(value);
                t.dynamic = true;
                continue;
            }
            value += c;
            ++m_i;
        }
        return t;
    }

    void readBacktick(QString& value) {
        const int end = m_s.indexOf(QLatin1Char('`'), m_i + 1);
        if (end < 0) {
            fail(QStringLiteral("a backtick is not closed"));
            m_i = m_s.size();
            return;
        }
        recurse(m_s.mid(m_i + 1, end - m_i - 1));
        value += m_s.mid(m_i, end + 1 - m_i);
        m_i = end + 1;
    }

    const QString& m_s;
    int m_i = 0;
    int m_depth;
    ShellInspection& m_out;
    QVector<Heredoc> m_heredocs;
    bool m_stop = false;

  public:
    static ShellInspection inspectPosixCommandAt(const QString& command, int depth);
};

const QSet<QString>& keywords() {
    static const QSet<QString> k = {
        QStringLiteral("if"),
        QStringLiteral("then"),
        QStringLiteral("else"),
        QStringLiteral("elif"),
        QStringLiteral("fi"),
        QStringLiteral("do"),
        QStringLiteral("done"),
        QStringLiteral("while"),
        QStringLiteral("until"),
        QStringLiteral("{"),
        QStringLiteral("}"),
        QStringLiteral("!"),
        QStringLiteral("time"),
        QStringLiteral("coproc"),
    };
    return k;
}

// Wrappers that run another program: the wrapper itself needs no allow-list
// entry, the program it runs does.
const QSet<QString>& wrappers() {
    static const QSet<QString> w = {
        QStringLiteral("env"),
        QStringLiteral("nice"),
        QStringLiteral("nohup"),
        QStringLiteral("timeout"),
        QStringLiteral("xargs"),
        QStringLiteral("exec"),
        QStringLiteral("command"),
        QStringLiteral("builtin"),
        QStringLiteral("stdbuf"),
        QStringLiteral("ionice"),
        QStringLiteral("setsid"),
    };
    return w;
}

// Options of each wrapper that take a separate value word.
bool wrapperOptionTakesValue(const QString& wrapper, const QString& opt) {
    static const QHash<QString, QStringList> v = {
        {QStringLiteral("env"), {QStringLiteral("-u"), QStringLiteral("-C"), QStringLiteral("-S")}},
        {QStringLiteral("nice"), {QStringLiteral("-n")}},
        {QStringLiteral("timeout"),
         {QStringLiteral("-s"),
          QStringLiteral("-k"),
          QStringLiteral("--signal"),
          QStringLiteral("--kill-after")}},
        {QStringLiteral("xargs"),
         {QStringLiteral("-n"),
          QStringLiteral("-I"),
          QStringLiteral("-L"),
          QStringLiteral("-P"),
          QStringLiteral("-d"),
          QStringLiteral("-s"),
          QStringLiteral("-a"),
          QStringLiteral("-E")}},
        {QStringLiteral("exec"), {QStringLiteral("-a")}},
        {QStringLiteral("ionice"),
         {QStringLiteral("-c"), QStringLiteral("-n"), QStringLiteral("-p")}},
    };
    return v.value(wrapper).contains(opt);
}

bool isShellInterpreter(const QString& p) {
    return p == QLatin1String("bash") || p == QLatin1String("sh") || p == QLatin1String("zsh") ||
           p == QLatin1String("dash") || p == QLatin1String("ksh");
}

QString basename(const QString& word) {
    const int slash = word.lastIndexOf(QLatin1Char('/'));
    return slash < 0 ? word : word.mid(slash + 1);
}

bool isAssignment(const QString& w) {
    static const QRegularExpression re(
        QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*(\\[[^]]*\\])?\\+?="));
    return re.match(w).hasMatch();
}

ShellInspection Lexer::inspectPosixCommandAt(const QString& command, int depth) {
    ShellInspection out;
    // Tokens up to the point the lexer could follow; anything it could not
    // parse is left to the shell.
    const QVector<Token> toks = Lexer(command, depth, out).run();

    auto recurseText = [&](const QString& text) {
        if (depth + 1 > kMaxDepth) {
            out.complete = false;
            return;
        }
        ShellInspection sub = inspectPosixCommandAt(text, depth + 1);
        out.programs.append(sub.programs);
        if (!sub.complete)
            out.complete = false;
        if (!sub.refusal.isEmpty() && out.refusal.isEmpty())
            out.refusal = sub.refusal;
    };

    bool atStart = true;
    bool skipUntilDo = false;        // inside "for x in ...;" / "select"
    bool caseHeader = false;         // between "case" and "in"
    int caseDepth = 0;               // open case statements
    bool inDoubleBracket = false;    // [[ ... ]]
    QString wrapper;                 // active wrapper whose options are being skipped
    bool wrapperNeedsValue = false;  // next word is a wrapper option's value
    bool timeoutNeedsDuration = false;
    bool afterPipe = false;  // the current simple command reads a pipe

    for (int k = 0; k < toks.size() && out.refusal.isEmpty(); ++k) {
        const Token& t = toks.at(k);
        if (t.kind == Token::Op) {
            if (t.text == QLatin1String(";;")) {
                atStart = true;
                afterPipe = false;
                continue;
            }
            afterPipe = (t.text == QLatin1String("|") || t.text == QLatin1String("|&"));
            atStart = true;
            wrapper.clear();
            wrapperNeedsValue = timeoutNeedsDuration = false;
            continue;
        }
        if (t.kind == Token::Redirect) {
            ++k;  // the target word (or heredoc delimiter)
            continue;
        }
        const QString& w = t.text;
        if (inDoubleBracket) {
            if (w == QLatin1String("]]"))
                inDoubleBracket = false;
            continue;
        }
        if (skipUntilDo) {
            if (w == QLatin1String("do")) {
                skipUntilDo = false;
                atStart = true;
            }
            continue;
        }
        if (caseHeader) {
            if (w == QLatin1String("in")) {
                caseHeader = false;
                atStart = true;
            }
            continue;
        }
        if (!atStart)
            continue;

        if (!wrapper.isEmpty()) {
            if (wrapperNeedsValue) {
                wrapperNeedsValue = false;
                continue;
            }
            if (w.startsWith(QLatin1Char('-'))) {
                if (wrapper == QLatin1String("command") &&
                    (w == QLatin1String("-v") || w == QLatin1String("-V"))) {
                    atStart = false;  // a lookup, not an execution
                    wrapper.clear();
                    continue;
                }
                wrapperNeedsValue = wrapperOptionTakesValue(wrapper, w);
                continue;
            }
            if (wrapper == QLatin1String("env") && isAssignment(w))
                continue;
            if (timeoutNeedsDuration) {
                timeoutNeedsDuration = false;
                continue;
            }
            wrapper.clear();  // this word is the program the wrapper runs
        }

        if (caseDepth > 0 && w.endsWith(QLatin1Char(')')))
            continue;  // case pattern
        if (w == QLatin1String("esac")) {
            if (caseDepth > 0)
                --caseDepth;
            continue;
        }
        if (w == QLatin1String("case")) {
            caseHeader = true;
            ++caseDepth;
            continue;
        }
        if (w == QLatin1String("for") || w == QLatin1String("select")) {
            skipUntilDo = true;
            continue;
        }
        if (w == QLatin1String("[[")) {
            inDoubleBracket = true;
            atStart = false;
            continue;
        }
        if (w == QLatin1String("function")) {
            ++k;
            continue;
        }
        if (keywords().contains(w))
            continue;
        if (!t.dynamic && isAssignment(w))
            continue;

        // name() { ...; }  — a function definition, not a call.
        if (k + 2 < toks.size() && toks.at(k + 1).kind == Token::Op &&
            toks.at(k + 1).text == QLatin1String("(") && toks.at(k + 2).kind == Token::Op &&
            toks.at(k + 2).text == QLatin1String(")")) {
            k += 2;
            continue;
        }

        // A program name taken from a variable ("$PYTHON x.py") is common
        // and harmless on its own; it is simply not identified.
        if (t.dynamic) {
            atStart = false;
            continue;
        }

        const QString prog = basename(w);
        atStart = false;

        if (wrappers().contains(prog)) {
            wrapper = prog;
            atStart = true;
            timeoutNeedsDuration = (prog == QLatin1String("timeout"));
            continue;
        }
        if (prog == QLatin1String("eval")) {
            QStringList parts;
            for (++k; k < toks.size() && toks.at(k).kind == Token::Word; ++k) {
                if (toks.at(k).dynamic) {
                    out.refusal = QStringLiteral(
                        "Refused: eval runs text built from variables or command output, "
                        "which cannot be checked before it runs. Run the commands directly.");
                    break;
                }
                parts << toks.at(k).text;
            }
            --k;
            if (out.refusal.isEmpty())
                recurseText(parts.join(QLatin1Char(' ')));
            continue;
        }
        if (!isHarmlessShellBuiltin(prog))
            out.programs << prog;

        if (isShellInterpreter(prog)) {
            // sh -c 'script' / bash -lc "script": inspect the script. A shell
            // with neither -c nor a script file reads its script from stdin.
            bool hasScript = false;
            for (int j = k + 1; j < toks.size() && toks.at(j).kind == Token::Word; ++j) {
                const QString& a = toks.at(j).text;
                if (!a.startsWith(QLatin1Char('-')) || a == QLatin1String("-")) {
                    hasScript = (a != QLatin1String("-"));  // a script file
                    break;
                }
                if (a.startsWith(QLatin1String("--")))
                    continue;
                if (a.contains(QLatin1Char('c'))) {
                    hasScript = true;
                    if (j + 1 >= toks.size() || toks.at(j + 1).kind != Token::Word)
                        break;
                    if (toks.at(j + 1).dynamic) {
                        out.refusal =
                            QStringLiteral(
                                "Refused: the script passed to %1 -c is built from variables or "
                                "command output, which cannot be checked before it runs.")
                                .arg(prog);
                        break;
                    }
                    recurseText(toks.at(j + 1).text);
                    break;
                }
            }
            if (!hasScript && afterPipe && out.refusal.isEmpty()) {
                out.refusal =
                    QStringLiteral(
                        "Refused: piping text into %1 runs code that cannot be checked before it "
                        "runs. Save it to a file and run the file, or run the commands "
                        "directly.")
                        .arg(prog);
            }
        }
    }
    return out;
}

}  // namespace

ShellInspection inspectPosixCommand(const QString& command) {
    return Lexer::inspectPosixCommandAt(command, 0);
}

bool isHarmlessShellBuiltin(const QString& name) {
    static const QSet<QString> b = {
        QStringLiteral("cd"),      QStringLiteral("pushd"),    QStringLiteral("popd"),
        QStringLiteral("dirs"),    QStringLiteral("export"),   QStringLiteral("unset"),
        QStringLiteral("set"),     QStringLiteral("shopt"),    QStringLiteral("echo"),
        QStringLiteral("printf"),  QStringLiteral("test"),     QStringLiteral("["),
        QStringLiteral("true"),    QStringLiteral("false"),    QStringLiteral(":"),
        QStringLiteral("pwd"),     QStringLiteral("read"),     QStringLiteral("exit"),
        QStringLiteral("return"),  QStringLiteral("local"),    QStringLiteral("declare"),
        QStringLiteral("typeset"), QStringLiteral("readonly"), QStringLiteral("shift"),
        QStringLiteral("wait"),    QStringLiteral("type"),     QStringLiteral("hash"),
        QStringLiteral("source"),  QStringLiteral("."),        QStringLiteral("alias"),
        QStringLiteral("unalias"), QStringLiteral("umask"),    QStringLiteral("trap"),
        QStringLiteral("let"),     QStringLiteral("break"),    QStringLiteral("continue"),
        QStringLiteral("jobs"),    QStringLiteral("getopts"),
    };
    return b.contains(name);
}

}  // namespace Verzeta
