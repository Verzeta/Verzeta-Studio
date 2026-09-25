<!--
SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
SPDX-License-Identifier: LGPL-3.0-or-later
-->

# Running Verzeta Studio as a headless server

Verzeta Studio can run in the background on a machine without a
display attached. Your phone and other paired devices connect to it
the same way they connect to a desktop session. Typical setups
include a home server, a NAS, a cloud VM, or a spare laptop kept
running in another room.

This page is the operator runbook. Use it if you are installing
Verzeta Studio on a server and managing it from another device.

If you only want to add Remote Access to your normal desktop
session, see [Remote access and paired clients](08-remote-android.md)
instead. **Settings → Remote Access → Manage Remote Access** does that
without any of the steps below.

## Before you start

You will need:

- A Linux machine you can reach over SSH.
- Verzeta Studio on that machine: the Linux AppImage from the release
  page, or a build from source (see BUILDING.md). The examples below
  assume the program is at `/usr/bin/verzeta-studio`; change the path
  to match yours.
- A local disk for the database. Network storage (NFS, SMB, SSHFS,
  cloud-mounted drives) is not supported and Verzeta will refuse to
  start on one.
- The Android app installed on the device you want to pair.

## The fastest path

If you already use Verzeta Studio on your desktop and have configured
Remote Access there, you can copy your existing settings to the
server and start in one step.

On your desktop:

```bash
scp ~/.config/Verzeta/verzeta-studio.conf \
    you@server:~/.config/Verzeta/verzeta-studio.conf
```

The copied file brings your bind address and port. It does not bring
your API keys (they only work on the computer where they were
entered), and it does not turn on TLS on the server: the desktop's
TLS switch is not used by `--headless-remote`. To use TLS, copy the
desktop's certificate files,
`~/.local/share/Verzeta/verzeta-studio/verzeta-remote/cert.pem` and
`key.pem`, to the server and start it with
`--remote-tls --remote-cert <path> --remote-key <path>` (see
[Enabling TLS](#enabling-tls)).

On the server:

```bash
QT_QPA_PLATFORM=offscreen verzeta-studio --headless-remote
```

Without TLS the server prints a warning that pairing codes and
tokens travel in plain text.

That is the whole quick start. The next section shows how to wrap
this in a systemd service so it survives reboots.

### Using the AppImage

An AppImage runs headless the same way:
`QT_QPA_PLATFORM=offscreen ./Verzeta-Studio-1.0.0-x86_64.AppImage --headless-remote`.
Point `ExecStart=` at the AppImage's full path.

The pairing helper `verzeta-remote` is inside the AppImage. To use it,
extract the AppImage once with
`./Verzeta-Studio-1.0.0-x86_64.AppImage --appimage-extract` and run
`squashfs-root/usr/bin/verzeta-remote --pair-code`.

## Running as a system service

Save the following to `~/.config/systemd/user/verzeta-studio-headless.service`:

```ini
[Unit]
Description=Verzeta Studio headless host
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=/usr/bin/verzeta-studio --headless-remote
Environment=QT_QPA_PLATFORM=offscreen
Restart=on-failure
RestartSec=5s

[Install]
WantedBy=default.target
```

Then:

```bash
systemctl --user daemon-reload
systemctl --user enable --now verzeta-studio-headless
systemctl --user status  verzeta-studio-headless
journalctl --user -u     verzeta-studio-headless -f
```

A user service stops when you log out. To keep it running after
logout and start it at boot, run `loginctl enable-linger $USER` once.

To run as a different user without that user being logged in, use a
system unit instead. Save the following to
`/etc/systemd/system/verzeta-studio-headless@.service`:

```ini
[Unit]
Description=Verzeta Studio headless host for %i
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=%i
Group=%i
ExecStart=/usr/bin/verzeta-studio --headless-remote
Environment=QT_QPA_PLATFORM=offscreen
Restart=on-failure
RestartSec=5s

[Install]
WantedBy=multi-user.target
```

Then enable it for a specific user:

```bash
sudo systemctl daemon-reload
sudo systemctl enable --now verzeta-studio-headless@alice
```

## Configuration

Verzeta Studio reads its settings from
`~/.config/Verzeta/verzeta-studio.conf`, a plain INI file that you
can edit. The desktop's Remote Access dialog saves the bind address
and port here. For headless TLS, add the `tlsEnabled`, `certPath` and
`keyPath` keys yourself or pass the command-line flags below.

The relevant section:

```ini
[remote]
bindAddr=0.0.0.0
port=9180
tlsEnabled=true
certPath=/home/alice/.local/share/Verzeta/verzeta-studio/verzeta-remote/cert/cert.pem
keyPath=/home/alice/.local/share/Verzeta/verzeta-studio/verzeta-remote/cert/key.pem
```

| Key          | Purpose                                                                                                        |
| ------------ | -------------------------------------------------------------------------------------------------------------- |
| `bindAddr`   | Address to listen on. Use `127.0.0.1` for loopback only or `0.0.0.0` to accept connections from any interface. |
| `port`       | TCP port to listen on. Default is `9180`.                                                                      |
| `tlsEnabled` | Set to `true` to require encrypted connections.                                                                |
| `certPath`   | Absolute path to the server certificate.                                                                       |
| `keyPath`    | Absolute path to the matching private key.                                                                     |

You can override any setting at the command line:

| Flag                   | Notes                                                    |
| ---------------------- | -------------------------------------------------------- |
| `--headless-remote`    | Required. Starts in background mode.                     |
| `--remote-bind <addr>` | Override the bind address.                               |
| `--remote-port <port>` | Override the TCP port.                                   |
| `--remote-tls`         | Enable TLS. Requires `--remote-cert` and `--remote-key`. |
| `--no-remote-tls`      | Disable TLS.                                             |
| `--remote-cert <path>` | Path to the server certificate.                          |
| `--remote-key <path>`  | Path to the private key.                                 |

Command-line flags win over the config file. The config file wins
over the built-in defaults.

The certificate the desktop app creates is at
`~/.local/share/Verzeta/verzeta-studio/verzeta-remote/cert.pem`, with
`key.pem` next to it.

## Using environment variables

For stateless setups managed by tools like Ansible or NixOS, you
can drive everything from environment variables in the unit file:

```ini
[Service]
Type=simple
Environment=QT_QPA_PLATFORM=offscreen
Environment=VERZETA_REMOTE_BIND=0.0.0.0
Environment=VERZETA_REMOTE_PORT=9180
Environment=VERZETA_REMOTE_CERT=/etc/verzeta/cert.pem
Environment=VERZETA_REMOTE_KEY=/etc/verzeta/key.pem
ExecStart=/usr/bin/verzeta-studio --headless-remote \
    --remote-bind ${VERZETA_REMOTE_BIND} \
    --remote-port ${VERZETA_REMOTE_PORT} \
    --remote-tls \
    --remote-cert ${VERZETA_REMOTE_CERT} \
    --remote-key  ${VERZETA_REMOTE_KEY}
Restart=on-failure
RestartSec=5s
```

You can also externalise the variables into a file:

```ini
[Service]
EnvironmentFile=/etc/verzeta/server.env
ExecStart=/usr/bin/verzeta-studio --headless-remote \
    --remote-bind ${VERZETA_REMOTE_BIND} \
    --remote-port ${VERZETA_REMOTE_PORT}
```

With `/etc/verzeta/server.env`:

```
VERZETA_REMOTE_BIND=0.0.0.0
VERZETA_REMOTE_PORT=9180
```

systemd expands the references before launching Verzeta Studio.

## Pairing a new device

Pairing codes are generated by a small helper command that talks to
the running server through a shared file. You do not need to stop
or restart the service to add a new device.

On the server:

```bash
verzeta-remote --pair-code
```

This prints a six-digit code. The code is valid for five
minutes and can be used once.

On your phone:

1. Open the Verzeta app and tap **Add host**.
2. Under **Endpoint**, enter the server address, ending in `/ws`, for
   example `wss://server:9180/ws`. If TLS is enabled, it starts with
   `wss://`. Otherwise it starts with `ws://`.
3. If TLS is enabled, paste the certificate fingerprint (see
   [Enabling TLS](#enabling-tls) below).
4. Enter the pair code.
5. Tap **Pair**.

After pairing, the device stores a token and reconnects
automatically on later sessions.

To remove a device:

```bash
verzeta-remote --list-clients
verzeta-remote --revoke <client-id>
```

A revoked device cannot reconnect until you pair it again.

## Enabling TLS

For any server reachable beyond your own computer, enable TLS. A
self-signed certificate with pinned fingerprints is fine for
servers on a LAN or VPN.

Generate a certificate and key:

```bash
mkdir -p ~/.local/share/Verzeta/verzeta-studio/verzeta-remote/cert
cd       ~/.local/share/Verzeta/verzeta-studio/verzeta-remote/cert
openssl req -x509 -newkey rsa:2048 -nodes -days 3650 \
            -keyout key.pem -out cert.pem \
            -subj "/CN=verzeta-server"
chmod 600 key.pem
```

Compute the fingerprint that the phone will pin:

```bash
openssl x509 -in cert.pem -fingerprint -sha256 -noout
```

The output looks like `sha256 Fingerprint=AB:CD:EF:01:23:45:...`.
Paste the part after `=` into the Android app's pairing form. The phone
will refuse to connect to any other certificate from then on.

Point the server at the certificate. Either set `certPath` and
`keyPath` in the config file, pass them on the command line, or
reference them via environment variables in the unit file.

To rotate the certificate, replace the files on disk and restart
the service. Paired devices with the old fingerprint will refuse to
connect until you give them the new fingerprint.

## Backups

The database lives at `~/.local/share/Verzeta/verzeta-studio/verzeta-studio.db`.

Do not copy it with `cp` while the service is running. Use the
built-in backup command instead:

```bash
sqlite3 ~/.local/share/Verzeta/verzeta-studio/verzeta-studio.db \
    ".backup /path/to/verzeta-studio.db.backup"
```

This produces a consistent snapshot you can move to another disk
or another machine. Run it on a schedule for periodic backups.

Other files worth backing up:

- `~/.local/share/Verzeta/verzeta-studio/` for all conversation data,
  canvases, RAG models, and so on.
- `~/.local/share/Verzeta/verzeta-studio/verzeta-remote/wire-state.db` for the bearer
  tokens of every paired device. Without this file, every device
  must be paired again.
- `~/.local/share/Verzeta/verzeta-studio/verzeta-remote/cert/` if you
  generated a self-signed certificate as shown above.
- `~/.config/Verzeta/verzeta-studio.conf` for your
  settings. API keys in it only work on this machine.

To restore, stop the service, replace the files, and start the
service again.

## Troubleshooting

### The server refuses to start because of the filesystem

Verzeta Studio refuses to open its database on network or
distributed filesystems. SQLite cannot run reliably over NFS, SMB,
SSHFS, or similar mounts.

Move your data directory to a local disk, or set `XDG_DATA_HOME`
to a local path:

```bash
XDG_DATA_HOME=/var/lib/verzeta-studio verzeta-studio --headless-remote
```

### Another instance is already running

A second Verzeta Studio process tried to use the same database
while the first one was still running. Check what is running:

```bash
ps -ef | grep verzeta-studio
systemctl --user status verzeta-studio-headless
```

Stop the existing instance before starting a new one. If a previous
process crashed, the next start cleans up after it on its own.

### The port is in use

Pick a different port with `--remote-port`, or find what is using
the existing one:

```bash
ss -tlnp | grep :9180
```

### TLS connection refused after rotation

After replacing the certificate, every paired device pinning the
old fingerprint refuses to connect. Update each device with the new
fingerprint from `openssl x509 -in cert.pem -fingerprint -sha256 -noout`.

### Pair code expired

Codes are single-use and valid for five minutes. Run
`verzeta-remote --pair-code` again to generate a fresh one.

### The service will not start

Check the journal:

```bash
journalctl --user -u verzeta-studio-headless -n 100
```

Common causes:

- The `ExecStart=` path in your unit file does not match the
  installed binary. Run `which verzeta-studio` and update the
  unit.
- The user the service runs as does not own
  `~/.local/share/Verzeta/verzeta-studio/`.
- The TLS certificate or key file is missing or unreadable.
- The port is already in use.

### Cloud provider API keys do not work under the service

API keys are stored in the settings of the user who entered them, not
in a system keychain. If the service runs as a different user, it reads
a different settings file and finds no keys.

Two workable approaches:

- Run the service as the same user who configured the keys.
- Skip cloud providers entirely. Run Verzeta against Ollama or
  another local provider that does not need an API key.

If the settings file was copied from another computer, the keys in it
do not work at all: keys only work on the computer where they were
entered. Enter them again on the server.

### I cannot find the configuration file

The file is `~/.config/Verzeta/verzeta-studio.conf` for the user the
service runs as:

```bash
ls -la ~/.config/Verzeta/
cat ~/.config/Verzeta/verzeta-studio.conf
```

If the file does not exist, Verzeta Studio has not been configured for
this user yet. Pass every setting on the command line, or run the app
with a display once to create it.
