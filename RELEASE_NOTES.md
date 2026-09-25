<!--
SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
SPDX-License-Identifier: LGPL-3.0-or-later
-->

# Release notes

Public release notes for Verzeta Studio. The release workflow publishes the
section whose heading matches the release version (`## [X.Y.Z]`) as the GitHub
release body. Format follows [Keep a Changelog](https://keepachangelog.com/).

## [1.0.0] - 2026-09-22

The first public release of **Verzeta Studio**, a local-first desktop workspace for running a team of AI agents in one conversation. Each agent has its own provider, model, role and tools.

### Highlights

- **Multi-agent group chats.** Name your agents and give each one its own provider, model, system prompt and tool whitelist. They `@mention` each other to hand off work.
- **Eight built-in providers, plus any OpenAI-compatible server.** OpenAI, Anthropic, Gemini, OpenRouter, DeepSeek, Ollama, llama.cpp over HTTP, and a built-in llama.cpp engine in the Local AI edition.
- **File, shell, web and canvas tools.** Agents read and write files, run the shell programs you allow, search the web, edit code in a live canvas, and work through multi-step task plans.
- **Clients for VS Code and Android.** Both connect to this host over your network. The VS Code client adds workspace mounts and consent-gated commands.
- **Skills, polls, an audit trail, and conversation memory** that keeps long group chats on track.

### Privacy

Your data stays on your machine unless you choose a cloud model provider. No cloud sync, no telemetry, no accounts.

### Downloads

Download the Linux AppImage or the Windows build from the assets below, or build from source. Clients: [Verzeta for VS Code](https://github.com/Verzeta/Verzeta-VSCode-Extension) and [Verzeta for Android](https://github.com/Verzeta/Verzeta-Android).
