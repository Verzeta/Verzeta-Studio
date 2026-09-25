<!--
SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
SPDX-License-Identifier: LGPL-3.0-or-later
-->

# Privacy Policy

This document describes what Verzeta™ Studio does with your data. It is written in plain language and applies to the open-source software distributed by the project; it does **not** describe what any third-party LLM provider you use does with the data you send them.

For the formal regulatory analysis (GDPR / HIPAA / PCI DSS posture), see [COMPLIANCE.md](COMPLIANCE.md).

---

## The short version

> **Verzeta Studio is local-first software.**
>
> - Your conversations, settings, attachments, and skills live on your own machine.
> - The developer (the publisher of this software) has **zero access** to your data.
> - The app has **no telemetry**, **no analytics**, **no cloud sync**, and no "phone home" of any kind.
> - Verzeta only connects to services **you** set up: model providers, paired devices, your web search backend, image and embeddings providers, MCP servers, ClawHub when you search for skills, and Hugging Face when you download a model.
> - Your data exists only on your device. Removing the app does not delete it; see "Delete everything" below.

If you read nothing else, that is the policy.

---

## Who runs Verzeta Studio

Verzeta Studio is open-source software distributed under a triple-license arrangement: GPL-3.0-or-later, LGPL-3.0-or-later, or commercial terms, depending on the component and the user's choice. The per-component breakdown is in [LICENSING.md](LICENSING.md). The publisher distributes the source code and binaries. Each user installs and runs the software on their own hardware.

There is no Verzeta-operated server, no Verzeta cloud account, and no Verzeta-side database holding user data. The publisher has no operational role in any individual user's installation.

This makes Verzeta Studio different from a typical Software-as-a-Service product: there is no central operator who could even theoretically access your data.

---

## What data Verzeta stores on your device

Verzeta stores the following on the device where you install it:

| Data                        | Stored where                                                                             | Purpose                                               |
| --------------------------- | ---------------------------------------------------------------------------------------- | ----------------------------------------------------- |
| Conversations and messages  | SQLite DB in `~/.local/share/Verzeta/verzeta-studio/` (Linux) or `%APPDATA%\Verzeta\verzeta-studio\` (Windows) | Persisting your chats                                 |
| Attachments (files, images) | Same data folder                                                                         | Persisting message attachments                        |
| Skills                      | `skills/` inside the data folder                                                         | Skill packs you have installed                        |
| Project documents           | Per-project folders inside the data folder                                               | Documents you attach to a project                     |
| Canvas artifacts            | Same data folder                                                                         | Files agents have opened in canvas                    |
| API keys                    | Obfuscated in the local settings file (`~/.config/Verzeta/verzeta-studio.conf` on Linux, the registry on Windows). Not encrypted. | Authenticating you to the LLM providers you configure |
| Logs                        | `logs/` inside the data folder                                                           | Local diagnostic logs; rotated                        |
| Paired-client tokens        | Local credentials database (only if you enable Remote Access)                            | Authenticating paired Android and VS Code clients     |
| Activity log                | Local database                                                                           | The audit trail of agent actions                      |

Every entry on this list is on **your machine** and is fully under your control. None of it is replicated anywhere else by Verzeta.

---

## What data Verzeta sends, and to whom

Verzeta initiates network traffic in only the following situations:

### 1. LLM provider API calls

When you send a message in a conversation, the message body, conversation history relevant to the request, system prompt, and any tool schemas are sent to the LLM provider you have configured for that conversation or member. This is the only way LLM chat works.

Supported providers:

- **Local providers** (Ollama on your machine or your LAN, the built-in llama.cpp engine). Your message stays on your network or your device.
- **Self-hosted servers** (llama.cpp Remote and custom OpenAI-compatible servers). Your message goes to the server you configured. Same trust boundary as any other server you run.
- **Cloud providers** (OpenAI, Anthropic, Google Gemini, OpenRouter, DeepSeek). Your message is sent to that provider's API. The provider receives the content and processes it per **their** privacy policy and terms.

> **Note**: When you use a cloud provider, you are agreeing to that provider's terms. Verzeta sends the provider what the model needs (the system prompt, recent messages, any summary or retrieved context, and tool definitions) and does not log or copy the request anywhere else. Each provider's privacy policy applies directly to your traffic with them. The publisher of Verzeta is not in the middle.

Each provider's privacy policy:

- OpenAI: <https://openai.com/policies/privacy-policy/>
- Anthropic: <https://www.anthropic.com/legal/privacy>
- Google Gemini: <https://ai.google.dev/gemini-api/terms>
- OpenRouter: <https://openrouter.ai/privacy>
- DeepSeek: <https://www.deepseek.com/privacy>
- Ollama: self-hosted; no provider privacy policy applies.
- llama.cpp: self-hosted; no provider privacy policy applies.

### 2. Remote pairing (optional)

If you enable **Remote Access** in Settings, Verzeta runs a separate daemon (`verzeta-remote`) on your machine that accepts WebSocket connections from paired devices (the Android and VS Code clients). Traffic between your desktop and paired devices is:

- Local-network or internet-routable depending on how you exposed the host URL.
- TLS-encrypted when you enable TLS (recommended; it is off by default). The Android client pins the server certificate by SHA-256 fingerprint.
- Authenticated by single-use pairing codes that issue per-client bearer tokens.

The data flowing over a remote-pairing connection is exactly the same data your local desktop holds. The phone is a remote front end to the desktop, not a duplicate runtime. No third party operates this connection.

### 3. Optional skill marketplace download

If you use the **ClawHub** integration to install a skill from a public catalog, Verzeta downloads the skill ZIP from the ClawHub URL you searched. The download is a normal HTTPS request. No identifying information beyond a standard User-Agent is sent. You can also install skills entirely from local folders without contacting ClawHub.

### 4. Web search (optional, agent-initiated)

If an agent calls the `search_web` tool, an HTTPS request is made to the configured web search backend. The agent's query is sent to the backend; the backend's results are returned to the agent. This traffic is the same shape as any other web search you do.

### 5. Model and voice downloads (optional)

When you click **Download recommended** or download a voice or speech model, Verzeta downloads the file from Hugging Face over HTTPS. Nothing about you or your conversations is sent.

### 6. Image, embeddings and MCP providers (optional)

If you set up an image provider, a remote embeddings endpoint, or an MCP server, Verzeta sends it the prompt, text or tool arguments it needs, and nothing else.

### What Verzeta does **not** transmit, ever

- Usage analytics or telemetry of any kind.
- The contents of your conversations to anyone other than the providers and services described above.
- Crash reports.
- Update check pings (the application does not auto-update).
- Anonymous heuristic data, fingerprints, or device identifiers.

---

## What Verzeta logs

Verzeta writes local diagnostic logs to its data folder so you can troubleshoot issues. Logs are written **only to your machine**.

Logs include things like:

- HTTP status codes from provider responses
- Timestamps of tool invocations
- Errors and warnings from the application's components
- Database migration steps

Logs are rotated by size; they do not grow indefinitely. Logs may include excerpts of error messages from providers, which may include sanitised request/response shapes. Logs do **not** record full API keys.

You can:

- View the logs (they are plain text).
- Delete the logs at any time.
- Turn on more detail for one run with the `QT_LOGGING_RULES` environment variable (see the Troubleshooting guide).

The publisher of Verzeta does not receive your logs unless you choose to attach them to a bug report you file yourself.

---

## Activity log (in-app audit trail)

Verzeta records every agent turn, tool invocation, poll vote, member change, canvas edit, and file write into a local `activity_log` table. It stays on your device:

- It lives in your local SQLite database.
- Only you (and any device you have paired) can read it.
- It is append-only by design so you have a complete audit trail of what your agents have done.
- You can delete rows at any time with any SQLite tool while the app is closed, or remove the database file.

See [Documentation/User/09-activity-timeline.md](Documentation/User/09-activity-timeline.md) for the full details.

---

## Your rights and how to exercise them

Because there is no central operator who holds your data, the rights data-protection regimes (GDPR / CCPA / etc.) grant data subjects are exercised by **you directly on your device**.

| What you may want to do                        | How to do it                                                                                                              |
| ---------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------- |
| Read all of my data                            | Open the app; browse conversations. The SQLite database is also readable with any SQLite browser.                         |
| Export my data                                 | Right-click the conversation in the sidebar → **Export…** (Markdown or JSON). The SQLite database can be copied directly. |
| Remove a conversation's messages               | Type `/flashmemory` in the chat, then `/flashmemory confirm`.                                                             |
| Delete a conversation                          | Right-click in the sidebar → **Delete**.                                                                                  |
| Delete everything                              | Close the app and delete the data folder and settings (`~/.local/share/Verzeta/verzeta-studio/` and `~/.config/Verzeta/verzeta-studio.conf` on Linux; `%APPDATA%\Verzeta\verzeta-studio\`, `%LOCALAPPDATA%\Verzeta\verzeta-studio\` and the registry key `HKEY_CURRENT_USER\Software\Verzeta\verzeta-studio` on Windows). |
| Stop transmitting to a particular provider     | Open **Settings → Text Providers**, click the provider, **Remove**. The API key is deleted; no further requests go there.  |
| Stop a paired device from accessing my desktop | **Settings → Remote Access → Manage Remote Access → Paired devices → Revoke**. The device can no longer connect.          |
| Disable the remote-access feature entirely     | In **Settings → Remote Access → Manage Remote Access**, turn off the server and **Auto-start at app launch**.             |

You do not need to contact the publisher for any of these. There is no publisher account to manage and no developer-side data to delete.

---

## Children's privacy

Verzeta Studio is not directed at children under 13 (or under any other applicable age threshold for child data protection laws in your jurisdiction). The application does not collect age information because it does not collect personal information at all.

If you are responsible for a minor's use of Verzeta Studio:

- Choose providers whose terms allow your minor's use. Cloud LLM provider terms vary; check each provider's age requirements.
- Consider local-only providers (Ollama or the built-in llama.cpp engine) which keep all data on the device.

---

## Cookies, tracking, and identifiers

Verzeta Studio is a native desktop and mobile application, not a website. It does not set cookies, run third-party JavaScript, or fingerprint the device.

The optional ClawHub skill search is the only feature that contacts a third-party service for content discovery, and the request is a plain HTTPS GET with a generic User-Agent.

---

## Changes to this policy

This policy is part of the source tree. Material changes are made via Git commits with explanatory messages. The Git history is the canonical record of every change.

If the application ever begins to collect or transmit any new category of data on the publisher's behalf, this policy must be updated **before** that change ships.

---

## Questions

- Day-to-day questions about how a feature handles data: see the relevant page in [Documentation/User/](Documentation/User/).
- Architectural and verification questions (how do I confirm there's no telemetry?): see [COMPLIANCE.md](COMPLIANCE.md).
- Security vulnerabilities: see [SECURITY.md](SECURITY.md) for responsible disclosure.
- Anything else: open an issue on the project's Git repository.

---

Last reviewed: 2026-09.
