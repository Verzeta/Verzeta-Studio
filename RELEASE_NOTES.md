<!--
SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
SPDX-License-Identifier: LGPL-3.0-or-later
-->

# Release notes

Public release notes for Verzeta Studio. The release workflow publishes the
section whose heading matches the release version (`## [X.Y.Z]`) as the GitHub
release body. Format follows [Keep a Changelog](https://keepachangelog.com/).

## [1.0.1] - 2026-09-28

### Fixed

- Clearing a provider's optional URL in Settings no longer breaks every request with a protocol error. An empty URL now means the provider's default endpoint.
- Image generation with OpenRouter works with every OpenRouter image model, including image-only models such as Flux, Seedream and Ming. Choose the new **OpenRouter** image provider type; the Base URL can be left empty.
- The Image Generation and Web Search steps of first-run setup no longer show a stray top bar with a Back button.
- When an agent is asked to save a file outside the project folder, nothing is written and it asks you first. Before, it quietly saved a renamed copy in the project folder and reported the original location.
- With write protection on, agents are told why a write was refused, so they stop and ask instead of retrying other commands.
- OpenRouter's activity log shows every request from Verzeta Studio under its own name and website, including image requests, which appeared as unknown.
- The skill review window keeps a normal size instead of stretching with the skill's text.
- Custom tools keep their command after a restart. Before, they silently stopped working.
- Values passed to custom tools are inserted as plain text and can no longer add commands of their own.

### Added

- The chat shows "Generating image" while an image is being made.
- First-run setup includes optional Image Generation and Web Search steps.
- Custom tools have a time limit setting (30 seconds by default).
- Optional write protection on Linux. When turned on in Execution & Permissions, commands agents run can only write inside the project, temporary folders and folders you choose.

### Changed

- Shell commands that pipe text into a shell, or run `eval` or `bash -c` on text built from variables, are refused, because what they run cannot be checked first.

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
