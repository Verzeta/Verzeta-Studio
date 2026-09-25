// SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
// SPDX-License-Identifier: LGPL-3.0-or-later


#include "utils/command-line-options.h"

#include <QTemporaryDir>
#include <QtTest>

#include <QObject>
#include <QSettings>
#include <QString>
#include <QStringList>
#include <QUuid>

namespace {

class ScopedSettings {
  public:
    explicit ScopedSettings(const QString& path) : m_settings(path, QSettings::IniFormat) {}
    QSettings& get() { return m_settings; }

  private:
    QSettings m_settings;
};

QString makeArgv0() {
    return QStringLiteral("verzeta-studio");
}

}  // namespace

class TestCommandLineOptions : public QObject {
    Q_OBJECT

  private slots:
    void init() {
        QVERIFY(m_tempDir.isValid());
        m_settingsPath =
            m_tempDir.path() + QStringLiteral("/settings_%1.ini")
                                   .arg(QUuid::createUuid().toString(QUuid::WithoutBraces));
    }

    void emptyArgs_emptySettings_yieldsDefaults() {
        ScopedSettings s(m_settingsPath);
        const auto opts = Verzeta::Utils::parseCommandLine({makeArgv0()}, s.get());

        QVERIFY(!opts.hasError);
        QVERIFY(!opts.headlessRemote);
        QCOMPARE(opts.remoteBind, QStringLiteral("0.0.0.0"));
        QCOMPARE(opts.remotePort, 9180);
        QVERIFY(!opts.remoteTls);
        QVERIFY(opts.remoteCert.isEmpty());
        QVERIFY(opts.remoteKey.isEmpty());
    }

    void cliSetsHeadlessRemote() {
        ScopedSettings s(m_settingsPath);
        const auto opts = Verzeta::Utils::parseCommandLine(
            {makeArgv0(), QStringLiteral("--headless-remote")}, s.get());
        QVERIFY(!opts.hasError);
        QVERIFY(opts.headlessRemote);
    }

    void cliSetsRemoteBindAndPort_inHeadlessMode() {
        ScopedSettings s(m_settingsPath);
        const auto opts = Verzeta::Utils::parseCommandLine({makeArgv0(),
                                                            QStringLiteral("--headless-remote"),
                                                            QStringLiteral("--remote-bind"),
                                                            QStringLiteral("127.0.0.1"),
                                                            QStringLiteral("--remote-port"),
                                                            QStringLiteral("9999")},
                                                           s.get());
        QVERIFY(!opts.hasError);
        QCOMPARE(opts.remoteBind, QStringLiteral("127.0.0.1"));
        QCOMPARE(opts.remotePort, 9999);
    }

    void cliSetsTlsWithCertAndKey() {
        ScopedSettings s(m_settingsPath);
        const auto opts = Verzeta::Utils::parseCommandLine({makeArgv0(),
                                                            QStringLiteral("--headless-remote"),
                                                            QStringLiteral("--remote-tls"),
                                                            QStringLiteral("--remote-cert"),
                                                            QStringLiteral("/etc/x/cert.pem"),
                                                            QStringLiteral("--remote-key"),
                                                            QStringLiteral("/etc/x/key.pem")},
                                                           s.get());
        QVERIFY2(!opts.hasError, qPrintable(opts.errorMessage));
        QVERIFY(opts.remoteTls);
        QCOMPARE(opts.remoteCert, QStringLiteral("/etc/x/cert.pem"));
        QCOMPARE(opts.remoteKey, QStringLiteral("/etc/x/key.pem"));
    }

    void qsettingsProvidesRemoteValuesWhenCliAbsent() {
        ScopedSettings s(m_settingsPath);
        s.get().setValue(QStringLiteral("remote/bindAddr"), QStringLiteral("192.168.1.5"));
        s.get().setValue(QStringLiteral("remote/port"), 9181);
        s.get().setValue(QStringLiteral("remote/tlsEnabled"), true);
        s.get().setValue(QStringLiteral("remote/certPath"), QStringLiteral("/persisted/cert.pem"));
        s.get().setValue(QStringLiteral("remote/keyPath"), QStringLiteral("/persisted/key.pem"));
        s.get().sync();

        const auto opts = Verzeta::Utils::parseCommandLine(
            {makeArgv0(), QStringLiteral("--headless-remote")}, s.get());
        QVERIFY2(!opts.hasError, qPrintable(opts.errorMessage));
        QVERIFY(opts.headlessRemote);
        QCOMPARE(opts.remoteBind, QStringLiteral("192.168.1.5"));
        QCOMPARE(opts.remotePort, 9181);
        QVERIFY(opts.remoteTls);
        QCOMPARE(opts.remoteCert, QStringLiteral("/persisted/cert.pem"));
        QCOMPARE(opts.remoteKey, QStringLiteral("/persisted/key.pem"));
    }

    void cliWinsOverQsettings() {
        ScopedSettings s(m_settingsPath);
        s.get().setValue(QStringLiteral("remote/bindAddr"), QStringLiteral("192.168.1.5"));
        s.get().setValue(QStringLiteral("remote/port"), 9181);
        s.get().sync();

        const auto opts = Verzeta::Utils::parseCommandLine({makeArgv0(),
                                                            QStringLiteral("--headless-remote"),
                                                            QStringLiteral("--remote-bind"),
                                                            QStringLiteral("127.0.0.1"),
                                                            QStringLiteral("--remote-port"),
                                                            QStringLiteral("7777")},
                                                           s.get());
        QVERIFY(!opts.hasError);
        QCOMPARE(opts.remoteBind, QStringLiteral("127.0.0.1"));
        QCOMPARE(opts.remotePort, 7777);
    }

    void cliNoTls_overridesPersistedTls() {
        ScopedSettings s(m_settingsPath);
        s.get().setValue(QStringLiteral("remote/tlsEnabled"), true);
        s.get().setValue(QStringLiteral("remote/certPath"), QStringLiteral("/persisted/cert.pem"));
        s.get().setValue(QStringLiteral("remote/keyPath"), QStringLiteral("/persisted/key.pem"));
        s.get().sync();

        const auto opts = Verzeta::Utils::parseCommandLine(
            {makeArgv0(), QStringLiteral("--headless-remote"), QStringLiteral("--no-remote-tls")},
            s.get());
        QVERIFY(!opts.hasError);
        QVERIFY(!opts.remoteTls);
    }

    void remoteFlagsWithoutHeadless_areRefused() {
        ScopedSettings s(m_settingsPath);
        const auto opts = Verzeta::Utils::parseCommandLine(
            {makeArgv0(), QStringLiteral("--remote-bind"), QStringLiteral("127.0.0.1")}, s.get());
        QVERIFY(opts.hasError);
        QVERIFY(opts.errorMessage.contains(QStringLiteral("--headless-remote")));
    }

    void tlsWithBothFlagsTogether_isRefused() {
        ScopedSettings s(m_settingsPath);
        const auto opts = Verzeta::Utils::parseCommandLine({makeArgv0(),
                                                            QStringLiteral("--headless-remote"),
                                                            QStringLiteral("--remote-tls"),
                                                            QStringLiteral("--no-remote-tls"),
                                                            QStringLiteral("--remote-cert"),
                                                            QStringLiteral("/x/c.pem"),
                                                            QStringLiteral("--remote-key"),
                                                            QStringLiteral("/x/k.pem")},
                                                           s.get());
        QVERIFY(opts.hasError);
        QVERIFY(opts.errorMessage.contains(QStringLiteral("--remote-tls")));
    }

    void tlsWithoutCert_isRefused() {
        ScopedSettings s(m_settingsPath);
        const auto opts = Verzeta::Utils::parseCommandLine(
            {makeArgv0(), QStringLiteral("--headless-remote"), QStringLiteral("--remote-tls")},
            s.get());
        QVERIFY(opts.hasError);
        QVERIFY(opts.errorMessage.contains(QStringLiteral("--remote-cert")));
    }

    void tlsWithoutKey_isRefused() {
        ScopedSettings s(m_settingsPath);
        const auto opts = Verzeta::Utils::parseCommandLine({makeArgv0(),
                                                            QStringLiteral("--headless-remote"),
                                                            QStringLiteral("--remote-tls"),
                                                            QStringLiteral("--remote-cert"),
                                                            QStringLiteral("/x/c.pem")},
                                                           s.get());
        QVERIFY(opts.hasError);
        QVERIFY(opts.errorMessage.contains(QStringLiteral("--remote-key")));
    }

    void invalidPort_isRefused() {
        ScopedSettings s(m_settingsPath);
        const auto opts = Verzeta::Utils::parseCommandLine({makeArgv0(),
                                                            QStringLiteral("--headless-remote"),
                                                            QStringLiteral("--remote-port"),
                                                            QStringLiteral("0")},
                                                           s.get());
        QVERIFY(opts.hasError);
        QVERIFY(opts.errorMessage.contains(QStringLiteral("--remote-port")));
    }

    void outOfRangePort_isRefused() {
        ScopedSettings s(m_settingsPath);
        const auto opts = Verzeta::Utils::parseCommandLine({makeArgv0(),
                                                            QStringLiteral("--headless-remote"),
                                                            QStringLiteral("--remote-port"),
                                                            QStringLiteral("99999")},
                                                           s.get());
        QVERIFY(opts.hasError);
    }

    void corruptedPersistedPort_fallsBackToDefault() {
        ScopedSettings s(m_settingsPath);
        s.get().setValue(QStringLiteral("remote/port"), -42);
        s.get().sync();

        const auto opts = Verzeta::Utils::parseCommandLine(
            {makeArgv0(), QStringLiteral("--headless-remote")}, s.get());
        QVERIFY(!opts.hasError);
        QCOMPARE(opts.remotePort, 9180);
    }

  private:
    QTemporaryDir m_tempDir;
    QString m_settingsPath;
};

QTEST_GUILESS_MAIN(TestCommandLineOptions)
#include "test-command-line-options.moc"
