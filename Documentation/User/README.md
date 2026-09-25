# Verzeta Studio user documentation

Welcome. This folder is the user guide for Verzeta Studio.

Pages 00 to 11 also appear inside the app via the **Help** button. All pages are on the project website and in the Git repository.

## Table of contents

| File                                               | Topic                                                                      | When to read                                                             |
| -------------------------------------------------- | -------------------------------------------------------------------------- | ------------------------------------------------------------------------ |
| [00-introduction.md](00-introduction.md)           | What Verzeta is, key benefits, use cases                                   | First. Sets the mental model.                                            |
| [01-quick-start.md](01-quick-start.md)             | 5-step walkthrough from install to first conversation                      | Right after the introduction.                                            |
| [02-providers.md](02-providers.md)                 | All 8 supported LLM providers and how to set each up                       | When configuring or switching providers.                                 |
| [03-conversations.md](03-conversations.md)         | Solo chat features, Chat Settings, slash commands, attachments, keyboard shortcuts | When using a single-agent conversation.                          |
| [04-multi-agent-teams.md](04-multi-agent-teams.md) | Group chats with several agents, agent templates                           | When you want to build a team of agents with different roles and models. |
| [05-projects.md](05-projects.md)                   | Project rooms, templates, shared team roster, project documents, heartbeats | When organising recurring work with a stable team.                      |
| [06-canvas-and-tasks.md](06-canvas-and-tasks.md)   | Live canvas editor and multi-step task plans                               | When agents need to draft files or drive multi-step work.                |
| [07-skills.md](07-skills.md)                       | Installable instruction packs, approval flow, preferred skills             | When adding domain knowledge to your agents.                             |
| [08-remote-android.md](08-remote-android.md)       | Pairing the Android and VS Code clients with the desktop                   | When you want to use your agents from your phone or your editor.         |
| [09-activity-timeline.md](09-activity-timeline.md) | The audit log: who did what, when                                          | When you need to inspect or verify agent activity.                       |
| [10-troubleshooting.md](10-troubleshooting.md)     | Common problems and how to fix them                                        | When something isn't working as expected.                                |
| [11-faq.md](11-faq.md)                             | Common questions about intended behaviour                                  | When you want to confirm how something is supposed to work.              |
| [12-self-hosted-servers.md](12-self-hosted-servers.md) | Running Verzeta against your own OpenAI-compatible servers            | When self-hosting models behind llama.cpp, vLLM, LM Studio etc.         |
| [13-conversation-memory.md](13-conversation-memory.md) | Context windows, the memory gauge, automatic compaction, /compact     | When conversations get long, or before your first big group session.    |
| [14-headless-server.md](14-headless-server.md)     | Running the backend without a GUI                                          | When deploying Verzeta on a server.                                      |
| [15-agent-execution-and-permissions.md](15-agent-execution-and-permissions.md) | The shell command allow-list, custom tools and MCP servers | When you want to control which programs and tools agents may use.        |
| [16-voice-calls.md](16-voice-calls.md)             | Talking to agents with the optional voice add-on                           | When you want to speak to your agents instead of typing.                 |

## Reading order

**If you are completely new**, read in number order, 00 to 16.

**If you are setting up for the first time**, read 00, then 01, then 02. The rest you can return to as needed.

**If you mainly want agent teams**, read 00, then jump to 04 and 05.

**If you are debugging an issue**, jump straight to 10.

## Reporting documentation issues

If something in this documentation does not match the app's behaviour, please open an issue on the Git repository. We treat it as a bug.

## License

This documentation is part of Verzeta Studio and is licensed under LGPL-3.0-or-later. The source code itself is under a triple-license arrangement: GPL-3.0-or-later for the differentiated components, LGPL-3.0-or-later for the supporting infrastructure, and a commercial license on request. Each file declares its own license in an SPDX header. See [LICENSING.md](../../LICENSING.md).
