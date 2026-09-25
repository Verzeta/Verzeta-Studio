<!--
SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
SPDX-License-Identifier: LGPL-3.0-or-later
-->

<div align="center">

<img src="resources/icons/verzeta-studio.png" width="128" alt="Verzeta Studio">

# Verzeta™ Studio

**Your AI team, running on your machine, in one chat.**

[![License: GPL-3.0+ / LGPL-3.0+ / Commercial](https://img.shields.io/badge/License-GPL%203.0%2B%20%2F%20LGPL%203.0%2B%20%2F%20Commercial-blue.svg)](LICENSING.md)
[![Platforms: Linux · Windows · Android](https://img.shields.io/badge/Platforms-Linux%20%C2%B7%20Windows%20%C2%B7%20Android-success.svg)](BUILDING.md)
[![Built with Qt 6 + Kirigami](https://img.shields.io/badge/Built%20with-Qt%206%20%E2%80%A2%20KDE%20Kirigami-orange.svg)](https://develop.kde.org/frameworks/kirigami/)
[![REUSE 3.0 compliant](https://img.shields.io/badge/REUSE-3.0-brightgreen.svg)](https://reuse.software/)

[![Website](https://img.shields.io/badge/Website-verzeta.com-2563eb.svg)](https://verzeta.com)
[![Follow @VerzetaAI](https://img.shields.io/badge/Follow-%40VerzetaAI-000000.svg?logo=x&logoColor=white)](https://x.com/VerzetaAI)
[![Reddit r/Verzeta](https://img.shields.io/badge/Reddit-r%2FVerzeta-FF4500.svg?logo=reddit&logoColor=white)](https://www.reddit.com/r/Verzeta/)

**[verzeta.com](https://verzeta.com) · [X / Twitter](https://x.com/VerzetaAI) · [Reddit](https://www.reddit.com/r/Verzeta/)**

</div>

Verzeta Studio is an open-source AI workspace where several agents, each using its own model, role, and tools, work together in one conversation. Run a researcher on Ollama locally, a writer on Claude in the cloud, and a reviewer on Gemini, all coordinating from the same chat.

Your conversations and settings stay on your machine unless you choose a cloud model provider. There is no cloud sync, no telemetry and no account.

---

## Why Verzeta

Verzeta Studio runs a team of AI agents in one chat.

- **Multi-agent group chats.** Several named agents per conversation, each with its own provider, model, system prompt, and tool whitelist. They `@mention` each other to hand off work.
- **Eight built-in LLM providers, plus any OpenAI-compatible server.** OpenAI, Anthropic (Claude), Google Gemini, OpenRouter, DeepSeek, Ollama (local), the llama.cpp HTTP server, and a built-in llama.cpp engine in the Local AI edition. Add as many vLLM, LM Studio, Jan, Llamafile, TabbyAPI, KoboldCpp, LocalAI, SGLang, or text-generation-webui servers as you want, and each becomes a provider any agent can route through.
- **Agent capabilities.** Agents read and write files, run the shell programs you allow (canvas scripts run in a bubblewrap sandbox on Linux), search the web, edit code in a side canvas, and drive multi-step task plans.
- **Wire clients for VS Code and Android.** Bring your agent teams into the editor you already work in, or onto your phone, over a WebSocket connection that you can encrypt with TLS. See [Use Verzeta everywhere](#use-verzeta-everywhere).
- **Workspace mounts.** Let the team browse, read, and write the real files in a connected VS Code workspace, and run shell commands in it. The bytes stay on your machine, and every write and command is consent-gated.
- **Conversation memory.** Long group chats stay coherent. The team summarizes and compacts older context automatically, so the details that matter stay in the window.
- **Skills.** Installable capability packs your agents discover and apply automatically. Every skill is approved by you before any agent can use it.
- **Polls.** When the team needs a decision, agents open a poll and vote on the record.
- **Full audit trail.** Every agent turn, tool call, file write, member change, and poll vote is recorded in a timeline you can browse and filter.
- **Local-first.** Conversations, attachments, and skills live in a local database on your device.

---

## Use Verzeta everywhere

Verzeta Studio is the **host**. It runs your models, tools, and agents. Lightweight **wire clients** connect to it over a WebSocket connection (TLS recommended), so your teams come with you. Nothing runs in the cloud; the clients talk to your own host.

### Verzeta for VS Code

Your project rooms, inside the editor: **[github.com/Verzeta/Verzeta-VSCode-Extension](https://github.com/Verzeta/Verzeta-VSCode-Extension)**

- Chat with your agent team from the **sidebar**, the editor-area panel, or a movable **editor tab**.
- Send the current **selection, files, or a git diff** to the team, and trigger **Explain, Fix, Refactor, Add tests, or Add docs** straight from the editor lightbulb.
- **Workspace mount.** Give the team read and write access to your open workspace. They work on your real project files (which never leave your machine), with writes approved per your chosen tier: **Ask**, **Smart**, or **Bypass**.
- **Sandboxed commands.** Let an agent run shell commands in the workspace (build, test, grep) to do real work, gated per conversation (**Off**, **Ask**, or **Allow**) and run inside bubblewrap on Linux when it is available.

### Verzeta for Android

Your conversations on the go. Pair your phone with your desktop and keep talking to the same teams from anywhere on your network: **[github.com/Verzeta/Verzeta-Android](https://github.com/Verzeta/Verzeta-Android)**

**Get the clients, downloads, and setup guides at [verzeta.com](https://verzeta.com).** Verzeta for VS Code and Verzeta for Android connect to the same host over the same wire protocol.

### Verzeta-Voice (add-on, coming soon)

Talk to your team out loud. An optional voice add-on gives one-to-one and group chats a call button: hold to talk, and the agents answer in their own voices, each one speaking before the next takes its turn. Speech recognition and synthesis run on your machine like everything else.

The add-on is not released yet. Verzeta Studio detects it at runtime and hides every voice control until it is installed, so nothing here is needed to use Verzeta.

---

## Get started

Verzeta Studio is open source. Each release publishes pre-built packages for Linux (an **AppImage**) and Windows (a portable **zip**), and you can build from source for any supported platform (see **[BUILDING.md](BUILDING.md)**).

Two editions are published for each platform:

- **Standard.** Connects to whatever providers you configure. That includes cloud services (OpenAI, Claude, Gemini) and any local server you run on your own machine or network, such as Ollama or a self-hosted llama.cpp server. You can stay fully offline this way by running a local server. This is the smaller download and the right choice for most people.
- **Local AI.** Everything in Standard, plus a built-in engine that runs local models directly, without installing a separate server. It uses your GPU where the hardware supports it and the CPU otherwise. This is the larger download, and its file name includes `localai`.

Both editions can run models entirely on your own machine and offline. The difference is only how: Standard talks to a model server you run yourself (added like any other provider), while Local AI adds an engine inside the app so you do not have to run a separate server. If you are unsure, choose **Standard**. You can switch editions later without losing your conversations or settings. For a per-file breakdown, see the [Quick start](Documentation/User/01-quick-start.md).

After launching, the **Get Started** wizard walks you through configuring providers and an introduction to multi-agent teams. On the **Local AI** edition it can also download a recommended model for you.

> **Fastest path to a working chat:** install [Ollama](https://ollama.com), pull a model (for example `ollama pull qwen3:8b`, or `qwen3:4b` on a machine with little memory), and leave the URL at the default. No API key required.

---

## Documentation

The complete user guide lives at **[Documentation/User/](Documentation/User/)**. Pages 00 to 11 are also available inside the app via the **Help** button.

| Topic                                                         | Page                                                                |
| ------------------------------------------------------------- | ------------------------------------------------------------------- |
| What Verzeta is and how the pieces fit together               | [Introduction](Documentation/User/00-introduction.md)               |
| A short walkthrough from launch to first conversation         | [Quick start](Documentation/User/01-quick-start.md)                 |
| All 8 LLM providers: setup and switching                      | [Providers](Documentation/User/02-providers.md)                     |
| Solo conversations, per-conversation settings, slash commands | [Conversations](Documentation/User/03-conversations.md)             |
| Multi-agent group chats and how routing works                 | [Multi-agent teams](Documentation/User/04-multi-agent-teams.md)     |
| Project folders, shared team rosters, project documents       | [Projects](Documentation/User/05-projects.md)                       |
| The live canvas editor and multi-step task plans              | [Canvas and tasks](Documentation/User/06-canvas-and-tasks.md)       |
| Installable capability packs and the approval flow            | [Skills](Documentation/User/07-skills.md)                           |
| Pairing wire clients (Android, VS Code) with the desktop      | [Remote access](Documentation/User/08-remote-android.md)            |
| The audit log: who did what, when                             | [Activity timeline](Documentation/User/09-activity-timeline.md)     |
| Connecting self-hosted and OpenAI-compatible model servers    | [Self-hosted servers](Documentation/User/12-self-hosted-servers.md) |
| How long conversations stay coherent: memory and compaction   | [Conversation memory](Documentation/User/13-conversation-memory.md) |
| Running Verzeta as a headless server                          | [Headless server](Documentation/User/14-headless-server.md)         |
| Which programs agents may run, and how to change the list     | [Agent execution and permissions](Documentation/User/15-agent-execution-and-permissions.md) |
| Talking to your agents with the optional voice add-on         | [Voice calls](Documentation/User/16-voice-calls.md)                 |
| Common issues with concrete fixes                             | [Troubleshooting](Documentation/User/10-troubleshooting.md)         |
| Frequently asked questions                                    | [FAQ](Documentation/User/11-faq.md)                                 |

---

## Privacy

Verzeta Studio is local-first by design:

- **No telemetry, no analytics.** The app never phones home.
- **No cloud sync.** Your conversations and settings stay on your device.
- **No accounts, no sign-in.** Verzeta does not have a central operator.
- **No publisher access.** There is no shared backend for the maintainer to read from.
- **API keys** are stored in your local settings file in obfuscated form. This is not encryption; protect your user account.

Full details: **[PRIVACY.md](PRIVACY.md)** · **[COMPLIANCE.md](COMPLIANCE.md)** · **[SECURITY.md](SECURITY.md)**

Security issues: please follow the responsible-disclosure process in [SECURITY.md](SECURITY.md). Do not open a public issue.

---

## Community

- **Website:** [verzeta.com](https://verzeta.com)
- **X / Twitter:** [@VerzetaAI](https://x.com/VerzetaAI)
- **Reddit:** [r/Verzeta](https://www.reddit.com/r/Verzeta/)

---

## Contributing

Bug fixes, features, documentation, tests, and translations are all welcome.

- **[CONTRIBUTING.md](CONTRIBUTING.md):** the contribution guide (style, testing, commit conventions, license-by-contribution).
- **[CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md):** we adopt the [Contributor Covenant v2.1](https://www.contributor-covenant.org/version/2/1/code_of_conduct/).

For bugs and feature ideas, open an issue on the project's Git repository.

---

## License

Verzeta Studio is released under a **triple-license** arrangement. Every source file declares its license inline via an `SPDX-License-Identifier` header, and the project is [REUSE 3.0](https://reuse.software/) compliant.

- **GPL-3.0-or-later** for the differentiated components: RAGP, the multi-agent cascade and routing layer, the task and project workflow primitives, the sandboxed canvas runner, and the heartbeat subagent loop. Derivative works distributed in binary form must be released under GPL-3.0 (or later) with corresponding source.
- **LGPL-3.0-or-later** for the supporting infrastructure: generic models, utilities, the wire-protocol implementation, UI components, design tokens, and build tooling. Permits dynamic linking into proprietary work under LGPL §6.
- **Commercial licenses** are available from the project owner for organisations that do not want either copyleft variant. Contact via the email in [SECURITY.md](SECURITY.md) for terms.

Canonical license texts live in [`LICENSES/`](LICENSES/). The triple-license arrangement and the per-component classification are explained in [LICENSING.md](LICENSING.md).

Contributions are accepted under a Contributor License Agreement that grants the project owner the right to relicense (necessary to operate the commercial track). See [CLA.md](CLA.md) and the contribution flow in [CONTRIBUTING.md](CONTRIBUTING.md).

Naming, attribution, and fork-naming guidance for the "Verzeta" name and logo are in [TRADEMARKS.md](TRADEMARKS.md).
