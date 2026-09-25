# Frequently asked questions

Common questions about Verzeta Studio's intended behaviour. For things that don't work the way you expect, see [10-troubleshooting.md](10-troubleshooting.md).

## 1. Where are my conversations stored?

All conversations, messages, attachments, tool results, polls, and activity log entries are stored in a local SQLite database file.

**Location:**

- **Linux**: `~/.local/share/Verzeta/verzeta-studio/verzeta-studio.db`
- **Windows**: `%APPDATA%\Verzeta\verzeta-studio\verzeta-studio.db`

The folder also holds project documents, canvas files, skills and logs. On Linux, downloaded GGUF models are stored there too; on Windows they are in `%LOCALAPPDATA%\Verzeta\verzeta-studio\`.

> **Pro-tip**: To back up your Verzeta data, close the app and copy the entire data folder. To move to a new computer, restore the folder there before opening the app, then enter your API keys again (they do not transfer; see question 6).

> **Note**: API keys are not in this folder. See question 6.

## 2. Are my prompts or conversations sent anywhere I haven't explicitly opted into?

No. Verzeta has no telemetry, analytics, usage reporting or cloud sync.

Your messages go only to places you set up:

- The model provider for the conversation or member (for example OpenAI, when a chat uses OpenAI).
- The `verzeta-remote` process on this computer, and from there your paired devices, if Remote Access is on.
- Your search backend, when an agent uses `search_web` (the search query only).
- Your embeddings endpoint, when RAG is on and set to Remote.
- Your image provider, when you or an agent generate an image (the prompt).
- MCP servers you add, when an agent calls one of their tools (the tool's arguments).

There are no analytics, no developer dashboards, and no Verzeta-operated servers.

> **Note**: When you use a cloud provider (OpenAI, Anthropic, Gemini, OpenRouter, DeepSeek), that provider receives the message content per their terms of service. This is the same as using any other API client. Verzeta sends the provider what the model needs to answer: the system prompt, recent messages, any summary or retrieved context, and tool definitions. It sends nothing else.

## 3. How do I switch the model used for one conversation?

Open the conversation. Open **Chat Settings** (the gear button in the chat header) and change the **Model** section.

The change applies to the next message you send. Previous messages in the conversation are not re-run.

If you want different agents within the same group conversation to use different models, see the per-member override section of [04-multi-agent-teams.md](04-multi-agent-teams.md). That feature lets you assign one provider to `@Alice` and another to `@Clark` in the same chat.

## 4. Can I run Verzeta completely offline?

Yes. There are three ways to run models locally, and you can mix them:

- **Ollama** (any edition): install Ollama, pull a model, and point Verzeta at `http://localhost:11434`. Once the model is pulled, chat needs no internet connection.
- **A self-hosted server** (any edition): run a model server such as llama.cpp, vLLM, or LM Studio on your own machine, then add it in Verzeta the same way as any other server (as a **llama.cpp (Remote)** provider, or as a custom OpenAI-compatible server). The model runs on your machine; this path does not use the built-in engine.
- **The built-in local engine** (the **Local AI** edition, the download whose file name contains `localai`): pick or download a `.gguf` model inside the app. No separate server to install.

What still requires connectivity even in "offline" mode:

- **The `search_web` tool**: uses the public internet by definition. Disable it or remove it from the agent's tool whitelist if you want an air-gapped setup.
- **Skills from ClawHub**: searching and downloading requires connectivity. Skills are local once installed.
- **Downloading recommended models**: the wizard and the RAGP and Embeddings pages download from Hugging Face.
- **Android remote pairing**: needs network reachability between desktop and phone, but only on your local network (no internet required for LAN pairing).

> **Note**: RAG works offline when **Settings → Providers → Embeddings (RAG)** uses a local model (Local AI edition) or an embeddings server on your own network.

## 5. What's the difference between a regular conversation and a group chat?

| Aspect               | Regular conversation                                | Group chat                                                       |
| -------------------- | --------------------------------------------------- | ---------------------------------------------------------------- |
| Number of agents     | One                                                 | Two or more                                                      |
| Speaker on each turn | Always the same assistant                           | Whichever member is dispatched (cascade decides)                 |
| @mention routing     | No: @mentions are plain text                        | Yes: @mentions dispatch to the named member                      |
| Member overrides     | One model per conversation                          | Per-member provider, model and tools                             |
| Polls                | Available but rarely used                           | Often used for coordinated decisions                             |
| Created via          | **New Chat**                                        | **New Group Chat**                                               |
| Best for             | Solo Q&A, drafting with one assistant, quick lookup | Multi-agent collaboration, team workflows, coordinated decisions |

Both kinds support the same per-conversation settings (system prompt, temperature, RAG, tools, etc.). The difference is whether several named members take part.

For the deep version, see [04-multi-agent-teams.md](04-multi-agent-teams.md).

## 6. Where are my settings and API keys stored?

Settings and API keys are stored separately from your conversations:

- **Linux**: `~/.config/Verzeta/verzeta-studio.conf`
- **Windows**: the registry, under `HKEY_CURRENT_USER\Software\Verzeta\verzeta-studio`

API keys are scrambled so they cannot be read at a glance, but they are not encrypted. Anyone who can read your user account's files can recover them. They only work on the computer where you entered them, so after moving to a new computer you must enter them again.

## 7. How do I remove all my data?

Deleting the AppImage or the Windows folder does not delete your data. To remove everything, close Verzeta Studio and delete:

- **Linux**: `~/.local/share/Verzeta/verzeta-studio/` and `~/.config/Verzeta/verzeta-studio.conf`
- **Windows**: `%APPDATA%\Verzeta\verzeta-studio\`, `%LOCALAPPDATA%\Verzeta\verzeta-studio\` (downloaded models), and the registry key `HKEY_CURRENT_USER\Software\Verzeta\verzeta-studio`

To delete only your conversations, use **Settings → Storage → Clear All Conversations**. This cannot be undone.

## 8. Can I switch between the Standard and Local AI editions?

Yes. Both editions use the same data folder and settings, so your conversations and providers carry over. The built-in llama.cpp engine and local embeddings only work in the Local AI edition.

## 9. Is there a list of keyboard shortcuts?

Yes. See [Keyboard shortcuts](03-conversations.md#keyboard-shortcuts).

## 10. Why did my agent team stop?

A team stops when it has nothing left to do, when a round made no progress, or when it has used up its automatic rounds (6 by default). The chat always posts a notice saying which. Reply with anything to start it again, or raise **Team autonomy: rounds before pausing** in **Chat Settings → Agent**. A single agent also pauses after 25 tool calls in one turn (**Tool steps per turn**). See [Turn limits, rounds, and pauses](04-multi-agent-teams.md#turn-limits-rounds-and-pauses).

## 11. Are agent shell commands sandboxed?

No. The shell tool only runs programs on your allow-list and refuses destructive patterns, but the commands themselves run with your user's permissions. Only canvas **Run** uses a sandbox (bubblewrap, on Linux). See [15-agent-execution-and-permissions.md](15-agent-execution-and-permissions.md).

## Where to read next

- [00-introduction.md](00-introduction.md): Overview of the project and its key benefits.
- [01-quick-start.md](01-quick-start.md): Walkthrough from install to first conversation.
- [10-troubleshooting.md](10-troubleshooting.md): Solutions to common issues.
