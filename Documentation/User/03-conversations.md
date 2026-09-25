# Conversations

A conversation is a single chat thread. This page covers solo conversations, per-conversation settings, slash commands, attachments and keyboard shortcuts. For multi-agent group conversations, see [04-multi-agent-teams.md](04-multi-agent-teams.md).

## Two kinds of conversations

| Kind                   | What it is                                                                                                                          |
| ---------------------- | ----------------------------------------------------------------------------------------------------------------------------------- |
| **Solo conversation**  | You and one AI model. One assistant; no agent cascade. The default for **New Chat**.                                                |
| **Group conversation** | You and a team of named agent members. Members @mention each other and replies cascade automatically. Created via **New Group Chat**. |

Both kinds have the same conversation settings. Group chats also have per-member settings.

## Creating a solo conversation

1. On the home screen, click **New Chat** (or press Ctrl+N).
2. The conversation opens with the default provider and model from your global settings.
3. Type your message. Press **Enter** to send, **Shift+Enter** for a newline.

## Per-conversation settings

Each conversation has its own settings, separate from the global defaults. Open **Chat Settings** (the gear button in the chat header, or Ctrl+Shift+S) to change them.

### Provider and model

Override the global default provider and model for this chat only, in the **Model** section. Useful when you want one conversation on cloud and another on local.

> **Pro-tip**: When you switch providers mid-conversation, the new provider is used for the next message. Earlier replies are not re-run.

### System prompt

A free-text instruction prefixed to every request sent on this conversation. The agent receives it as the system role.

> **Note**: The system prompt is **conversation-scoped**, not global. Other conversations are unaffected.

### Temperature

A value from 0.0 to 2.0 that controls randomness. New conversations use the model's own default until you move the slider.

- `0.0`: most predictable.
- `0.7`: a common value for chat.
- `1.0` and above: more varied and less predictable.

### Max tokens

Upper bound on the assistant's response length. **Auto (no cap)**, the default for new conversations, lets the model decide its own stopping point. This is usually what you want, especially with thinking models whose reasoning shares the same budget. Explicit values are passed to providers that support the parameter. The built-in llama.cpp engine is the exception: on **Auto** it caps a reply at 1,024 tokens.

### Streaming

When enabled (default), tokens stream into the bubble as they arrive. When disabled, the full reply appears at once. Disable only if your provider does not support streaming (rare).

### Thinking mode

For models that support a separate reasoning pass before answering (qwen3, DeepSeek-R1, Claude with extended thinking, Gemini 2.5, OpenAI o1 / o3, gpt-oss-thinking, etc.), the **Thinking** toggle (in the message bar, or **Thinking Mode** in Chat Settings) tells the model whether to reason before it produces the visible reply. Off by default.

- **On**: the model thinks before answering. Replies are usually slower but tend to be sharper on planning, multi-step problems, careful tool-call selection, and anything that benefits from internal deliberation.
- **Off**: the model skips the reasoning pass (where the provider supports an explicit switch) and answers directly. Faster and lighter, well suited to small talk and obvious questions.

The reasoning the model produces is rendered as a **collapsed Reasoning disclosure** inside the assistant bubble: a single muted row labelled `▸ Reasoning (N chars)` above the reply. Click the row to expand and see the full thinking text in a muted scrollable panel; click again to collapse. The disclosure appears only when the model produced reasoning; replies from other models look the same as before.

When the model produces reasoning **but no visible reply** (some reasoning models go silent after a long thinking pass on a hard follow-up to a tool result), the bubble carries an italic muted note ("The model produced reasoning only. Expand below to see it.") and the disclosure starts expanded so you can read the reasoning.

Reasoning is shown to you only. It is never re-sent to the model on later turns (so it doesn't double your per-turn context cost), never indexed for search, and not included in exports. The toggle asks the model to reason before answering, and any reasoning it produces is shown in the collapsible disclosure.

Verzeta sends the toggle to every provider that exposes a per-request thinking parameter, choosing the right shape per provider:

| Provider                                                                                                                | What the toggle does                                                                                                                                                 |
| ----------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Ollama                                                                                                                  | Sends `think: true \| false`. Reasoning models honour it. For a model without thinking support, Ollama may return an error when the toggle is on; turn it off for that model. |
| Claude (Anthropic)                                                                                                      | Sends an `enabled` / `disabled` thinking block on Claude 3.7 Sonnet and the Claude Opus 4 and Sonnet 4 families; other Claude models ignore the toggle.              |
| Gemini                                                                                                                  | Drives the thinking budget on Gemini 2.5 family models; older Geminis ignore the toggle.                                                                             |
| OpenAI                                                                                                                  | Sets reasoning effort on o1 and o3 models; non-reasoning OpenAI models ignore the toggle.                                                                            |
| OpenRouter                                                                                                              | Sends the standardised reasoning hint, routed to whichever upstream is serving the request.                                                                          |
| llama.cpp server / vLLM / LM Studio / Jan / Llamafile / TabbyAPI / KoboldCpp / LocalAI / SGLang / text-generation-webui | Passes `enable_thinking` to the loaded model's chat template; reasoning-capable templates (qwen3 family, DeepSeek-R1, gpt-oss-thinking) honour it, others ignore it. |
| DeepSeek                                                                                                                | No per-request switch. `deepseek-reasoner` always reasons; `deepseek-chat` never does. The toggle has no effect; pick the model that matches what you want.         |
| llama.cpp (built-in engine)                                                                                             | No per-request switch. Whether the loaded GGUF reasons depends on its chat template; the toggle has no effect.                                                      |

### Use app-recommended sampling

Some models ship with factory settings that misbehave in long
multi-agent chats (the best-known case: Qwen 3.5/3.6 on Ollama cutting
replies off mid-sentence). For those models Verzeta carries a tested
sampling recipe and applies it automatically. This checkbox (visible
only when a recipe exists for your current model) controls that.

- **Checked (default):** the recipe overrides the advanced sliders
  below at send time. Recommended.
- **Unchecked:** your own slider values win. The recipe only fills in
  sliders you left on **Auto**.

Beneath it sit the **advanced sampling sliders** (Top-K, Top-P,
Repeat / Presence / Frequency penalty), each with an **Auto** position
that means "use the model's own default". Leave them on Auto unless you
know what you're tuning. See
[Troubleshooting](10-troubleshooting.md) for the Qwen story.

### Context window

The model context size Verzeta budgets requests against (and passes to
Ollama as its window size). Bigger windows fit more conversation
before compaction is needed. Group chats should use 16k or more when
the model and hardware allow.

### Tools

When on (the default), the model can call tools (files, shell, web search, MCP tools). When off, tools are hidden from the model for this conversation. You can also switch this with the **Tools** toggle in the message bar.

### Tool steps per turn

How many tool calls an agent may chain in one turn before it pauses and asks you to continue. Default 25; 0 means unlimited. This setting applies to all conversations. A separate check always stops a turn after 5 tool calls fail in a row.

### Describe tools in system prompt

Default **off**. Models receive their tool definitions through a
structured channel either way; this switch also adds a written list
of every tool to the agent's instructions. That written list is
redundant for modern models and costs a meaningful slice of every
request (roughly 3,000 tokens on a full tool roster) that is better
spent on conversation history. Turn it **on** for a conversation only
if a smaller model keeps "forgetting" its tools; some behave better
with the written list. Conversations created before this default
changed keep whatever setting they had; flip the switch in Chat
Settings to update them.

### Dynamic compaction

Default **on**. Verzeta summarises the older part of a long
conversation in the background (*before* the model's context window
runs out) so decisions and assignments stay in the model's view.
Your visible transcript is never altered. A
`~Dynamic Compact Performed~` entry appears in the chat each time.

Beneath the toggle, **"Refresh summary every N agent replies"**
(default 20, 0 = off) also refreshes the summary on a fixed
rhythm, whether or not the context is full. This helps because
long team exchanges drift even when everything still fits.

Full explanation, the context gauge, and the manual `/compact` and
`/flashmemory` commands: see
[Conversation memory](13-conversation-memory.md).

### Auto-complete tasks when the model goes quiet

Default **off**. When on, an open task is marked done automatically if the model stops right after a tool call. Leave it off for multi-part work, so the task stays open until someone completes it. See [06-canvas-and-tasks.md](06-canvas-and-tasks.md).

### Agent pattern

Affects how the conversation orchestrates the assistant. The **Pattern** list in the **Agent** section shows these values:

| Pattern       | What it does                                                                  |
| ------------- | ----------------------------------------------------------------------------- |
| `direct`      | One model call per message. Default.                                          |
| `react`       | A reasoning and action loop with tool calls. Suited to tool-heavy tasks.      |
| `planner`     | Plans first, then carries out the plan. Suited to multi-step goals.           |
| `router`      | Picks one of the other strategies based on the message.                       |
| `multi_agent` | An older multi-agent mode. Use **New Group Chat** for teams instead.          |
| `memory`      | Searches earlier messages for context before replying.                        |

> **Note**: Most people should keep `direct`. The other patterns are experimental.

### Require confirmation

When enabled, you are asked to confirm every tool call before it runs. Useful when an agent has shell or file write access and you want to check each step yourself.

### Team autonomy: rounds before pausing

In **group chats**, how many autonomous rounds the team runs (agents
taking turns and @mentioning each other) before pausing to check in
with you. **Default 6**; raise it (up to 50) for more independence, or
set it to **Unlimited** to let the team keep going until it stops making
progress on its own. Lower it to steer the team every couple of
exchanges. A round ends and a new one begins only when the team made
real progress (a tool call, a correction, or a new contribution); a
round that only repeats earlier messages pauses. See
[Multi-agent teams → Turn limits, rounds, and pauses](04-multi-agent-teams.md#turn-limits-rounds-and-pauses).

### Preferred skills

Choose which approved skills agents in this chat are pointed to, with **Preferred Skills…**. See [07-skills.md](07-skills.md).

### Heartbeat

Controls whether scheduled heartbeat reports may be posted into this chat. See [Heartbeats](05-projects.md#heartbeats-scheduled-work).

### RAG

When **Enable RAG** is on, Verzeta searches this conversation's indexed messages and your knowledge-base documents for relevant context, and includes the most relevant passages in the system prompt. Off by default. You can also switch it with the **RAG** toggle in the message bar.

**Clear RAG memory** deletes the retrieval index built for this conversation. Your messages are not deleted. See [Conversation memory](13-conversation-memory.md#embeddings-and-retrieval-rag).

### Agent memory and team memory

Whether agents recall facts they saved, and whether this chat shares a project's team memory. See [Conversation memory](13-conversation-memory.md#agent-memory-saving-facts-to-remember).

## Slash commands

Type a slash command in the input field instead of a message. Known commands run locally and are not sent to the model. Text that starts with `/` but is not a known command is sent as a normal message.

| Command         | What it does                                                                                   |
| --------------- | ---------------------------------------------------------------------------------------------- |
| `/help`         | Lists available slash commands.                                                                |
| `/clear`        | Removes slash-command output (such as `/help` or `/showtools` results) from the view. Your conversation and what the model sees are unchanged. |
| `/artifacts`    | Shows the folder where this conversation's generated files are saved.                          |
| `/showtools`    | Shows a summary of the tools the model can use in this conversation, and how to filter the list (for example `/showtools builtin`, `/showtools custom` or `/showtools mcp`). |
| `/showmcptools` | Lists tools registered by external MCP servers (the same as `/showtools mcp`).                 |
| `/compact`      | Summarise the older part of this conversation now (non-destructive; same as clicking the circular context gauge in the message bar). See [Conversation memory](13-conversation-memory.md). |
| `/flashmemory`  | **Destructive**: wipes all messages, the summary and canvas state of this conversation after you confirm with `/flashmemory confirm`. The conversation itself (title, members, settings) survives. |

> **Pro-tip**: Use `/showtools` to check which tools an agent in this chat can use.

## Attachments

You can attach files to a message. The supported behaviour depends on the type:

- **Images** (PNG, JPEG, WebP, ...): sent to vision-capable providers (OpenAI GPT-4o, Claude, Gemini) as base64-encoded image content alongside your text.
- **Text-like files** (plain text, source code, JSON, Markdown): the content is read inline and appended to your message. The model sees it as text in the user message.

Use the **Attach files** button in the message bar.

> **Warning**: Attachments count toward the conversation's context window. Large files may push earlier messages out of context.

## Conversation actions

| Action         | Where                                                                                    |
| -------------- | ---------------------------------------------------------------------------------------- |
| Rename         | Right-click the conversation in the sidebar → **Rename…**                                |
| Move to folder | Right-click in the sidebar → **Move to Folder…**                                         |
| Pin            | Right-click in the sidebar → **Pin to top**                                              |
| Delete         | Right-click in the sidebar → **Delete**                                                  |
| Export         | Right-click in the sidebar → **Export…**, or Ctrl+E. Markdown or JSON, saved to your Downloads folder. |

## Message actions

- **Copy**: select text in any message and press Ctrl+C.
- **Retry**: the **Retry last response** button in the message bar sends your last message again for a new reply.
- **Stop**: while a reply is streaming, the **Send** button becomes **Stop**, which cancels the reply.

Sent messages cannot be edited.

## Keyboard shortcuts

| Keys | Action |
| --- | --- |
| Enter | Send the message |
| Shift+Enter | New line in the message |
| Ctrl+N | New chat |
| Ctrl+H | Go to the Home page |
| Ctrl+, | Open Settings |
| Ctrl+Shift+S | Show or hide the Chat Settings panel |
| Ctrl+Shift+C | Show or hide the canvas (when the chat has one) |
| Ctrl+E | Export the current conversation |

## Storage

All conversations and messages are stored locally in `~/.local/share/Verzeta/verzeta-studio/verzeta-studio.db` on Linux, or `%APPDATA%\Verzeta\verzeta-studio\verzeta-studio.db` on Windows. The database is a regular SQLite file; you can back it up by copying the folder while the app is closed.

> **Note**: No conversation content is ever sent anywhere except the provider you address the request to, and the services you set up yourself (see question 2 in [the FAQ](11-faq.md)). There is no telemetry, no usage reporting, and no cloud sync.

## What's next

- [04-multi-agent-teams.md](04-multi-agent-teams.md): group chat with multiple agents
- [05-projects.md](05-projects.md): organising conversations into projects
- [09-activity-timeline.md](09-activity-timeline.md): viewing the audit trail of a conversation
