// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
//
// SPDX-License-Identifier: GPL-3.0-or-later

/**
 * @file dangerous-pattern-scanner.cpp
 * @brief Implementation of the destructive-pattern scanner.  Catches
 *        accidental damage from agent-generated scripts.  Not a
 *        security boundary against adversarial input -- a determined
 *        attacker can bypass regex with creative quoting / encoding.
 * @layer Utility
 * @dependencies Qt6::Core (QString, QRegularExpression, QList).
 *               No service deps.
 */

#include "dangerous-pattern-scanner.h"

#include <QList>
#include <QRegularExpression>
#include <QString>
#include <QStringList>

namespace Verzeta {

namespace {

/**
 * @brief One pattern entry: a regex, a category label, and a one-line
 *        user-readable rejection reason. Categories are documented
 *        so the rejection log clearly says WHY the script was
 *        refused without leaking implementation details.
 */
struct PatternEntry {
    QRegularExpression rx;
    const char* category;
    const char* reason;
};

/**
 * @brief Build the master pattern list. Each entry uses the
 *        case-insensitive flag so common case-shifts (DEL vs del)
 *        don't bypass.
 *
 *        DotMatchesEverythingOption is intentionally OFF -- we want
 *        per-line behavior so a benign first line doesn't shadow a
 *        dangerous later line.
 *
 *        Patterns deliberately err on the side of false positives:
 *        for the "accidental damage" use case, blocking a few
 *        legitimate scripts is acceptable; letting an `rm -rf /`
 *        through is not. Users who need an actually-blocked pattern
 *        can split it across two scripts or run it manually outside
 *        the agent.
 */
QList<PatternEntry> buildPatterns() {
    const auto opts =
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::MultilineOption;

    QList<PatternEntry> list;
    auto add = [&](const QString& pat, const char* cat, const char* msg) {
        list.append(PatternEntry{QRegularExpression(pat, opts), cat, msg});
    };

    // ---------- Recursive deletion (Linux + macOS) ----------
    // `rm -rf` and friends. Matches `rm -r`, `rm -rf`, `rm -fr`,
    // `rm --recursive --force`, including with paths.
    add(QStringLiteral(R"(\brm\s+(-[rRfFdv]*[rRfFdv]+|--recursive|--force)\b)"),
        "destructive-delete",
        "Recursive rm detected, so the script was blocked. Verzeta "
        "Studio does not run commands that recursively delete files. "
        "If you need one, run it yourself "
        "outside the app.");

    // ---------- Recursive deletion (Windows) ----------
    // `del /S` and `rd /S` (recursive). `del /Q` adds quiet, often
    // paired. `rmdir` is the long form of rd.
    add(QStringLiteral(R"(\b(del|erase)\s+(/[sSqQfF]\s*)+)"),
        "destructive-delete-windows",
        "Recursive del/erase detected, so the script was blocked.");
    add(QStringLiteral(R"(\b(rd|rmdir)\s+(/[sSqQ]\s*)+)"),
        "destructive-delete-windows",
        "Recursive rd/rmdir detected, so the script was blocked.");

    // ---------- PowerShell recursive delete ----------
    // `\b-Recurse\b` won't match: `-` is a non-word char and the
    // char before it (a space) is also non-word, so there's no
    // word boundary. Use `(?:^|\s)-Recurse` anchored on whitespace
    // (or string start) instead.
    add(QStringLiteral(R"(\bRemove-Item\b.*(?:^|\s)-Recurse\b)"),
        "destructive-delete-powershell",
        "Remove-Item -Recurse detected, so the script was blocked.");

    // ---------- Privilege escalation ----------
    add(QStringLiteral(R"(\bsudo\b)"),
        "privilege-escalation",
        "sudo detected. Verzeta Studio does not run commands that "
        "need elevated privileges. Run them yourself in "
        "a terminal.");
    add(QStringLiteral(R"(\bsu\s+-)"),
        "privilege-escalation",
        "su - detected. Privilege escalation is not allowed.");
    add(QStringLiteral(R"(\bpkexec\b)"),
        "privilege-escalation",
        "pkexec detected. Privilege escalation is not allowed.");
    add(QStringLiteral(R"(\bdoas\b)"),
        "privilege-escalation",
        "doas detected. Privilege escalation is not allowed.");
    add(QStringLiteral(R"(\brunas\b)"),
        "privilege-escalation",
        "runas detected. Windows privilege escalation is not allowed.");
    add(QStringLiteral(R"(Start-Process.*-Verb\s+RunAs)"),
        "privilege-escalation",
        "PowerShell RunAs verb detected. Privilege escalation is not allowed.");

    // ---------- Disk format / raw device writes ----------
    add(QStringLiteral(R"(\bmkfs(\.\w+)?\b)"),
        "filesystem-format",
        "mkfs detected. Formatting a filesystem is not allowed.");
    add(QStringLiteral(R"(\bdd\s+.*\bof=/dev/(sd|nvme|mmcblk|hd))"),
        "raw-disk-write",
        "dd writing to a raw block device detected. This is not allowed.");
    add(QStringLiteral(R"(>\s*/dev/(sd|nvme|mmcblk|hd)\w*)"),
        "raw-disk-write",
        "Redirect to a raw block device detected. This is not allowed.");
    add(QStringLiteral(R"(\bformat\s+[a-zA-Z]:)"),
        "filesystem-format-windows",
        "Windows format command detected. This is not allowed.");
    add(QStringLiteral(R"(\bdiskpart\b)"),
        "filesystem-format-windows",
        "diskpart detected. It can repartition disks, so it is not allowed.");

    // ---------- Writes to system paths ----------
    // Linux/macOS protected paths -- /etc, /usr, /sys, /proc, /boot,
    // /System (macOS). Match common write idioms: shell redirect,
    // `rm`, `chmod`, `mv`, `cp`, `tee` writing into them.
    add(QStringLiteral(R"(>\s*/(etc|usr|sys|proc|boot|sbin|System)/)"),
        "system-path-write",
        "Write to a system path detected. This is not allowed.");
    add(QStringLiteral(
            R"(\b(chmod|chown|mv|cp|tee|cat\s+>>?)\s+.*?/(etc|usr|sys|proc|boot|sbin|System)/)"),
        "system-path-write",
        "Change to a system path detected. This is not allowed.");

    // Windows protected paths.
    add(QStringLiteral(R"(>\s*[A-Za-z]:[/\\](Windows|Program\s+Files(\s+\(x86\))?|System32))"),
        "system-path-write-windows",
        "Write to a Windows system path detected. This is not allowed.");
    add(QStringLiteral(R"(\b(copy|xcopy|robocopy|move)\s+.*\b[A-Za-z]:[/\\]Windows)"),
        "system-path-write-windows",
        "Change to C:\\Windows detected. This is not allowed.");

    // ---------- User / authentication tampering ----------
    add(QStringLiteral(R"(\b(passwd|useradd|userdel|usermod|chpasswd)\b)"),
        "auth-tamper",
        "User account modification refused.");
    add(QStringLiteral(R"(\b(net\s+user|net\s+localgroup)\b)"),
        "auth-tamper-windows",
        "Windows net user / net localgroup refused.");

    // ---------- Service / system control ----------
    add(QStringLiteral(R"(\b(systemctl|service)\s+(stop|disable|mask|kill))"),
        "service-control",
        "Stopping/disabling system services refused.");
    add(QStringLiteral(R"(\bsc\.exe\s+(stop|delete|config))"),
        "service-control-windows",
        "Windows sc service control refused.");
    add(QStringLiteral(R"(\bStop-Service\b|\bStop-Computer\b|\bRestart-Computer\b)"),
        "service-control-powershell",
        "PowerShell Stop-Service / Stop-Computer / Restart-Computer "
        "refused.");
    add(QStringLiteral(R"(\bshutdown\s+(-[hHrRsS]|/[sSrR])\b)"),
        "system-shutdown",
        "shutdown command refused.");
    add(QStringLiteral(R"(\breboot\b)"), "system-shutdown", "reboot command refused.");

    // ---------- Network admin ----------
    add(QStringLiteral(R"(\biptables\s+(-F|--flush|-X))"),
        "network-admin",
        "iptables -F / -X (firewall flush) refused.");
    add(QStringLiteral(R"(\bnetsh\s+(advfirewall|interface|wlan)\s)"),
        "network-admin-windows",
        "Windows netsh admin command refused.");

    // ---------- Kernel-module / driver ----------
    add(QStringLiteral(R"(\b(insmod|rmmod|modprobe)\b)"),
        "kernel-module",
        "Kernel module operation refused.");

    // ---------- Registry write (Windows) ----------
    add(QStringLiteral(R"(\breg\s+(add|delete|import|copy)\b)"),
        "registry-write",
        "Windows registry modification refused.");
    add(QStringLiteral(
            R"(\b(Set-ItemProperty|New-ItemProperty|Remove-Item)\b.*\bHK(LM|CU|CR|U|CC)\b)"),
        "registry-write-powershell",
        "PowerShell registry modification refused.");

    // ---------- Curl-pipe-to-shell (very common attack pattern) ----------
    add(QStringLiteral(R"(curl\s+[^|]*\|\s*(sh|bash|zsh|dash))"),
        "curl-pipe-shell",
        "curl|sh detected. Piping downloaded content straight into a "
        "shell is not allowed. Download the file, inspect it, then "
        "run it yourself.");
    add(QStringLiteral(R"(wget\s+[^|]*\|\s*(sh|bash|zsh|dash))"),
        "wget-pipe-shell",
        "wget|sh detected. This is not allowed.");
    add(QStringLiteral(R"(Invoke-(WebRequest|RestMethod).*\|\s*(Invoke-Expression|iex)\b)"),
        "powershell-iex-fetch",
        "PowerShell IEX of fetched content refused.");

    // ---------- Fork bomb ----------
    add(QStringLiteral(R"(:\(\)\s*\{\s*:\s*\|\s*:\s*&\s*\}\s*;\s*:)"),
        "fork-bomb",
        "Fork bomb pattern detected. This is not allowed.");

    return list;
}

const QList<PatternEntry>& patterns() {
    static const QList<PatternEntry> kList = buildPatterns();
    return kList;
}

}  // anonymous namespace

ScanResult scanForDangerousPatterns(const QString& content) {
    ScanResult out;
    out.allowed = true;

    if (content.isEmpty()) {
        return out;
    }

    for (const PatternEntry& entry : patterns()) {
        const QRegularExpressionMatch m = entry.rx.match(content);
        if (m.hasMatch()) {
            out.allowed = false;
            out.reason = QString::fromUtf8(entry.reason);
            out.pattern = m.captured(0);
            return out;
        }
    }

    return out;
}

}  // namespace Verzeta
