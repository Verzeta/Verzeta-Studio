# Introduction to Verzeta Studio

## What it is

Verzeta Studio is a local-first AI chat workspace where several AI agents work together as a team in one conversation. Each agent has its own model, role, system prompt and tool list.

The app is open source under a triple-license arrangement (GPL-3.0-or-later for the differentiated components, LGPL-3.0-or-later for the supporting infrastructure, and a commercial license for organisations that want neither copyleft variant). It runs on Linux and Windows, and pairs with two wire clients, Verzeta for Android and Verzeta for VS Code, over a connection you can encrypt with TLS.

## Key benefits

- **Multi-agent group chat.** Run multiple AI agents in one conversation. Each agent can use a different provider (e.g. Alice on Ollama locally, Bob on Claude, Carol on Gemini) and a different tool whitelist. They route work to each other with an `@mention`.

- **Local-first.** Your conversations, attachments, and skills are stored on your own machine in a SQLite database. API keys stay in your local settings in obfuscated form (see [how keys are stored](02-providers.md#storage-details)). Local models via Ollama and llama.cpp run on your hardware without sending data to anyone.

- **Eight providers supported out of the box.** Cloud: OpenAI, Anthropic, Gemini, OpenRouter, DeepSeek. Self-hosted servers: Ollama, llama.cpp (HTTP). Built-in: a local llama.cpp engine in the Local AI edition. Any OpenAI-compatible server can be added as well.

- **Real agent capabilities.** Agents can read and write files, run allowed shell commands, search the web, edit code in a side canvas, run multi-step tasks, use the skills you approve, and vote in polls. Every action is recorded in the activity log.

- **Skills.** Installable instruction packs (a `SKILL.md` file with frontmatter, plus optional supporting files). Agents discover them and apply them when relevant. Each skill is approved by you before any agent can use it.

- **Polls.** When the team needs a decision, they open a poll instead of looping in discussion. Votes are recorded, and the chat keeps moving.

- **Activity timeline.** Every agent turn, tool call, file write, member change, and poll vote is recorded. Open the timeline for a project or a single conversation to see what happened.

- **Mobile companion.** Pair an Android device with your desktop using a one-time code. Continue chats on your phone over a WebSocket connection, encrypted with TLS when you turn it on.

## Use cases

- **Drafting a document.** A writer agent drafts, a reviewer agent critiques, and a researcher agent fact-checks. Each step lands as a real file in the project's artifact folder.

- **Code review.** A coder agent edits a file in the canvas, and a reviewer agent runs it and writes a feedback note. The plan tracks unfinished steps.

- **Research with audit trail.** A search agent uses the web and a summariser agent writes notes. Every search query, every file read, and every conclusion is logged.

- **Team decisions.** When two agents disagree, the coordinator opens a poll. The team votes. The decision is recorded on the chat timeline.

## Product context

| Attribute        | Value                                                                                         |
| ---------------- | --------------------------------------------------------------------------------------------- |
| License       | GPL-3.0-or-later, LGPL-3.0-or-later, or commercial, declared per file. See [LICENSING.md](../../LICENSING.md) |
| Source code   | Open source. Browse and modify under GPL or LGPL, per the SPDX header on each file            |
| Platforms     | Linux and Windows                                                                             |
| Wire clients  | Verzeta for Android and Verzeta for VS Code                                                   |
| Telemetry     | None. The app never sends usage data anywhere.                                                |
| Data location | `~/.local/share/Verzeta/verzeta-studio/` on Linux, `%APPDATA%\Verzeta\verzeta-studio\` on Windows             |
| Connectivity  | Required only for cloud providers, web search, downloads and remote pairing                   |

## Adapting to your screen

Every dialog has the same layout: a title with an icon and a close button, an optional search box or subheading, and action buttons along the bottom.

On large screens dialogs open as centred cards. On small or touch screens they expand to fill the window, and you can drag a dialog down from its title strip to dismiss it.

## Where to read next

- **New user?** Start with [01-quick-start.md](01-quick-start.md) to get a working chat.
- **Want to run a team of agents?** [04-multi-agent-teams.md](04-multi-agent-teams.md) covers the multi-agent workflow.
- **Need to set up multiple providers?** [02-providers.md](02-providers.md) covers all 8.
- **Pairing a phone?** [08-remote-android.md](08-remote-android.md) covers pairing and what Android can and cannot do.
