# Remote access and paired clients

Verzeta has an optional remote access feature: you pair an Android device (or the VS Code extension) with your desktop, and the client acts as a remote front end to the desktop's chat. The Android app does not run AI models or tools locally. All work happens on the desktop; the phone shows the result.

## Architecture in one paragraph

Remote access runs as a **separate process** alongside the desktop app: a program called `verzeta-remote`. It hosts a WebSocket server that paired clients connect to. The desktop and the remote process talk over a local IPC socket. A crash in the remote process cannot affect the desktop; a desktop crash leaves paired clients with a temporary disconnect.

## What you need

- The Verzeta Studio desktop app, running.
- The Verzeta Studio Android app installed on your phone.
- Both devices on the same network (or the desktop reachable from the phone through a port or hostname you set up).

## Setting up remote access

### 1. Turn on remote access on the desktop

1. Open **Settings → Remote Access → Manage Remote Access**.
2. (Recommended) Turn on **TLS encryption** first. Verzeta creates a self-signed certificate and shows its **SHA-256 fingerprint**. You will enter this fingerprint on the phone. Creating the certificate needs the `openssl` command.
3. Turn on the server switch. The status changes to **Server is running**.
4. Note the address under **Clients can reach this host at**, for example `wss://192.168.1.42:9180/ws` with TLS or `ws://192.168.1.42:9180/ws` without it.

If you change the TLS setting while the server is running, turn the server off and on again. To start the server whenever Verzeta Studio opens, turn on **Auto-start at app launch**.

> **Warning**: TLS is off until you turn it on. Without TLS, all traffic between desktop and phone is unencrypted, including the pairing code. Use TLS unless you have a specific reason not to (and even then, only on a trusted local network).

### 2. Generate a pairing code

1. Under **Pair a device**, click **Generate pair code**.
2. A 6-digit code appears. It works once and expires after **5 minutes**.

### 3. Add the host on Android

1. Open the Verzeta Android app.
2. Tap **Add host**.
3. Enter a **Host name** of your choice, and under **Endpoint** enter the address shown on the desktop, including `/ws`.
4. If the address starts with `wss://`, paste the fingerprint from the desktop into **TLS certificate fingerprint (SHA-256)**. The Android client pins the certificate by this fingerprint; a mismatched certificate is refused.
5. Enter the **Pair code**.
6. Tap **Pair**.

The Android client exchanges the code for a bearer token. The token is stored on the device and used for future connections, so you do not need to re-pair every session.

### 4. Open a conversation on Android

After pairing, the Android client lists every conversation the desktop has access to. Tap one to open. Messages stream live; you can send messages, attach images, switch conversations, edit per-conversation settings, view the activity timeline, manage members, and trigger task plans, the same as on the desktop.

## What Android can do

- View every conversation, folder, and project from the paired desktop.
- Send and receive messages with live streaming.
- Attach images and text files.
- Override per-conversation settings (provider, model, system prompt, etc.).
- Manage group chat members (add, remove, set per-member overrides).
- View the activity timeline for any project or conversation.
- Vote in polls.
- Trigger task plans and review plan progress.

## How Android runs alongside the desktop

The Android client is a thin remote front end by design. Every heavyweight component (AI inference, tool execution, canvas Run, skill storage, classifier routing) lives on the desktop. The phone streams the result. This keeps the app small and light on battery.

When an agent calls a tool, the tool runs on the desktop and the result streams back to the phone. When you switch models from the phone, the desktop's model router handles it. When you open the canvas on the phone, the canvas content streams from the desktop's editor.

## Verzeta for VS Code

The VS Code extension pairs with the desktop the same way: turn on the server, generate a pair code, and enter the host address, fingerprint (if TLS is on) and code in the extension. It adds two things Android does not have:

- **Workspace mount.** Agents can read and write the files in your open VS Code workspace. Writes are approved according to the tier you choose in the extension (**Ask**, **Smart** or **Bypass**).
- **Commands in the workspace.** Agents can run shell commands in the workspace, gated per conversation (**Off**, **Ask** or **Allow**). These commands run on the VS Code machine under the extension's own rules.

See the extension's README for setup details.

## Per-client sessions

When you pair multiple devices with one desktop, each device gets its **own session**. Two phones can hold two different conversations open at the same time without each seeing the other's view. Streaming from one conversation does not leak into the other.

> **Pro-tip**: This makes it safe to pair a personal phone and a work phone with the same desktop. Each phone sees its own selected conversation; switching on one does not affect the other.

## Cross-instance routing: one in-flight LLM request at a time

Verzeta serialises foreground LLM requests across all sessions (desktop and paired clients) to avoid clobbering the model router's state. When one session has a request in flight, other sessions' sends are queued. The queued message appears with a small "Queued" indicator in the chat. When the in-flight session releases the slot, the queued send goes out automatically.

You will mostly notice this with slow local models or when several devices send at once.

## Revoking access

If you lose a paired device (or want to revoke it):

1. On the desktop, open **Settings → Remote Access → Manage Remote Access**.
2. Under **Paired devices**, find the device by its name or when it was last seen.
3. Click **Revoke**. The device's token stops working immediately. Revoked devices are listed under **View revoked devices**.

The device's next connection attempt is refused. You can pair the device again with a fresh code.

## Trust and device pairing

A paired device has the same access as sitting at the desktop, so pair only devices you trust.

The Android app can store more than one desktop. Add each one with **Add host** and pair it separately.

## What's next

- [03-conversations.md](03-conversations.md): Conversation features that work the same on desktop and Android.
- [05-projects.md](05-projects.md): Project workspaces visible from both ends.
- [09-activity-timeline.md](09-activity-timeline.md): The audit log on Android.
- [14-headless-server.md](14-headless-server.md): Running the host on a machine without a display.
