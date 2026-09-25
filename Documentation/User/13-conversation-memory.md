<!--
SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
SPDX-License-Identifier: LGPL-3.0-or-later
-->

# Conversation memory and compaction

Every model has a fixed amount of text it can pay attention to at once:
its **context window**. A long conversation eventually grows past
that limit, and something has to give. This page explains what Verzeta
Studio does when that happens, how to read the **memory gauge** in the
message bar, and which controls you have.

## The short version

- **Nothing you see is ever deleted.** Every message stays in the
  conversation and on screen, always.
- When the conversation outgrows the model's context window, Verzeta
  **summarises the older part automatically** and sends the model that
  summary plus the most recent messages. Decisions, open questions and
  who-said-what survive; filler does not.
- Each time this happens you'll see a small system entry in the chat:
  `~Dynamic Compact Performed, Reason: Auto~`. That's your receipt;
  the agents also see it, so they know a summary is in play.

## How this differs from "compact" in Claude Code or Codex

If you've used coding assistants, you may know `/compact` as something
that **replaces your visible conversation** with a summary: the old
messages disappear from the transcript. **Verzeta's compaction never
touches what you see.** The full transcript stays intact and scrollable
forever; compaction only changes what is *packaged and sent to the
model* behind the scenes.

## The memory gauge in the message bar

Next to the Send button you'll find a small **two-ring gauge**. The two
rings show the two things that can trigger an automatic memory refresh,
and **whichever ring fills first** is what triggers it:

- **Outer ring, the memory refresh schedule.** Fills as the team takes
  turns, reaching full at the conversation's "Refresh summary every N
  agent replies" setting. When it completes, a summary is refreshed.
- **Inner ring, the context window.** Fills as your instructions, the
  agents' tools, any existing summary, and recent messages use up the
  model's context window.

Each ring is colour-coded by how full it is: grey up to 35%, green to
65%, amber to 85%, orange to 95%, red beyond. **Hover the gauge** for
both numbers in plain words, for example *"Outer ring, Memory refresh: ~3
turns until the next refresh (5/8). Inner ring, Context window: 88%
full."* If the per-conversation refresh schedule is turned off, the
outer ring is idle and only context pressure drives a refresh.

The gauge starts filling after the first reply in the conversation, and
**clicking it compacts right now**. That is identical to typing `/compact`:
never destructive, and repeatable as often as you like.

## When auto-compaction kicks in

Three independent triggers can start a summary, and all three are
careful never to fire repeatedly for the same content:

1. **Early warning (the main one).** When the inner (context) ring
   reaches the amber zone (~70% full), a summary is generated **before
   anything stops fitting**, so the model never hits a point where
   older messages silently vanish without a summary already covering
   them.
2. **Conversation rhythm (configurable).** Long team exchanges drift
   even when everything still fits, and agents slowly lose the thread.
   The **"Refresh summary every N agent replies"** setting (default
   20, set 0 to disable) refreshes the summary on that rhythm so the
   team's working memory stays sharp. Set it higher for slow,
   deliberate conversations or lower for fast-moving ones.
3. **Backstop.** If messages are already being left out of the
   model's view and no up-to-date summary covers them, one is
   generated immediately.

In every case the summary is generated **in the background**, so the
conversation never waits for it. The reply you're watching streams
normally; from the *next* message onward the model receives the
summary. The newest ~15 messages are always sent verbatim, never
summarised. The summary is added alongside the messages that still
fit. It never replaces messages that would otherwise be sent.

The summary is rebuilt from scratch if the team roster changes or you
move the conversation to a different project (so it never describes a
stale cast of characters). Running tasks and background sub-agents
are safe across compaction: their state lives outside the message
history, and the summary explicitly preserves assignments, task
status, and pending sub-agent runs.

## Your controls

| Control | Where | What it does |
|---|---|---|
| **Dynamic compaction** toggle | Chat Settings | Default **on**. Turn it off and Verzeta falls back to plain truncation: when the window fills, the oldest messages are left out of what the model sees (they stay visible to you). |
| **Refresh summary every N agent replies** | Chat Settings | Default **20**. The conversation-rhythm trigger above. 0 = refresh only under context pressure. |
| **Memory gauge** / `/compact` | Message bar / typed command | Summarise now, on demand. Non-destructive, repeatable. |
| `/flashmemory` | Typed command | **Destructive.** Permanently deletes every message, the summary, and canvas state of the current conversation: a true fresh start. The conversation itself (title, members, settings) survives. You must confirm by typing `/flashmemory confirm`. |
| **Context Window** | Chat Settings | The size of the model's window Verzeta budgets against. Bigger = more conversation fits before compaction is needed, if your hardware and model support it. |

## Tips for long group chats

- **Raise the Context Window.** Group chats carry team instructions
  and tool definitions in every request; on small windows those alone
  eat most of the space. 16k or more is a good baseline for teams.
- **Check "Describe tools in system prompt" is off** (Chat
  Settings). It now defaults off, but conversations created before
  the default changed keep their old setting. Off frees a large
  chunk of every request for actual conversation. See
  [Conversations](03-conversations.md).
- **Watch the gauge colour.** Amber is your early warning; compacting
  manually at a natural pause (end of a work phase) gives the summary
  a clean cut-off point.

## Embeddings and retrieval (RAG)

Separately from compaction, the app can pull *relevant* earlier
messages and documents back into a turn even after they've scrolled
out of the live window. This is **retrieval** (often called RAG), and
it's **off by default**.

- **Turn it on per conversation** with the **RAG** button in the
  message bar or **Chat Settings → RAG → Enable RAG**. Once on, new messages in that
  conversation are indexed as you go. It does not re-index old history;
  the index builds up from when you enable it.
- **Pick how text is embedded** in **Settings → Providers → Embeddings (RAG)**:
  - **Local**: on-device embedding, nothing leaves your machine.
    Requires the Local AI edition; in the Standard edition the option
    says it is not in this build, so use Remote instead. When available, click **Add model…**,
    browse to any `.gguf` embedding model on your disk, and it is
    added and selected in one step, as on the RAGP page. **Download
    recommended** fetches a tested model from Hugging Face. (You can still drop a file into the shown
    folder by hand and press Refresh if you prefer.)
  - **Remote**: any OpenAI-compatible embeddings endpoint: OpenAI, or
    a local Ollama / LM Studio server. (Anthropic has no embeddings
    endpoint, so it can't be used here.)
- **Add documents** to the knowledge base from the same Settings
  section, so the team can recall from files as well as chat history.
- **Clear RAG memory** in **Chat Settings → RAG** deletes the index built
  for a conversation. Your messages are not deleted.

Retrieval runs in the background and never blocks your messages. If
the embedder is slow or unreachable, the turn still sends, without the
extra context.

## Agent memory (saving facts to remember)

Compaction and retrieval are about *this* conversation's history. **Agent
memory** is different: it lets an agent deliberately *save a durable fact*
and *recall it later*, including in other conversations.

- **Saving is the agent's choice; recall is automatic.** An agent that has
  the **`remember`** tool saves things worth keeping ("the user prefers metric
  units", "the staging server is called atlas"). It doesn't have to ask to
  recall: relevant memories are surfaced to it automatically before each
  reply. The agent decides what to remember.
- **On/off per conversation.** Agent memory recall is on by default; you can
  turn it off for a conversation in **Chat Settings → Agent memory**.
- **Memory is private to each agent.** What one agent remembers is its own;
  another agent in the same team doesn't see it. An agent carries its memory
  across every conversation it's in, so it stays consistent over time.
- **It works with or without embeddings.** Recall uses a fast text search,
  so it works even if you haven't set up an embedder. (Configuring an
  embedder in **Settings → Providers → Embeddings (RAG)** adds smarter, meaning-based recall
  over time.)
- **Controlling it.** Saving is done by the `remember` tool, so you control which agents
  have it through the per-member **tool
  whitelist**, the same way you control any tool (see [Multi-agent teams](04-multi-agent-teams.md)). Restrict a
  member's tools and you can leave memory out.
- **Deleting cleans up.** Deleting an agent removes everything it
  remembered; deleting a conversation removes anything saved in a plain
  one-to-one chat that had no named agent.

## Team memory (shared across a project)

Inside a **project or organization**, the team can build a **shared memory**
that any chat in that project can draw on.

- **It builds itself at compaction.** Whenever a chat in the project is
  compacted, its summary is added to the project's team memory, with no extra
  work and no extra cost. Over time the project accumulates a recallable record
  of what was discussed and decided.
- **Shared across the project's chats.** A point captured in one chat can be
  recalled in another chat in the same project. That is the difference from
  agent memory, which is private to one agent.
- **Two switches.** Turn it on or off for the whole project in **Folder
  Settings → Team memory** (on by default). Each individual chat in the
  project can also opt out in **Chat Settings → Team memory**. It only
  applies to chats inside a project or organization. Plain chats do not have it.
- **Deleting the project removes its team memory.**

## What's next

- [Conversations](03-conversations.md): all per-conversation settings.
- [Multi-agent teams](04-multi-agent-teams.md): how group chats,
  turn-taking and sub-agents work.
