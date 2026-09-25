# Providers

Verzeta Studio supports **eight** LLM providers across three categories. Configure as many as you need, and switch between them per conversation or per team member.

## The three categories

| Category            | Providers                                       | What they are                                                           |
| ------------------- | ----------------------------------------------- | ----------------------------------------------------------------------- |
| **Cloud Providers** | OpenAI, Anthropic, Gemini, OpenRouter, DeepSeek | Hosted APIs. Paste an API key and you are ready.                        |
| **Remote Servers**  | Ollama, llama.cpp (Remote HTTP)                 | Servers you run yourself, on this machine or another on your network.   |
| **Local Models**    | llama.cpp (built-in engine)                     | GGUF models run by an engine that ships with the Local AI edition. No server to install. |

Where to configure: **Settings → Text Providers**.

> **Note**: API keys and base URLs are stored only on this computer. They are sent to no one except the provider they belong to. See [Storage details](#storage-details).

## Cloud providers

### OpenAI

- **What it provides**: GPT models (e.g. GPT-4o, GPT-4o-mini). Your OpenAI key can also be used for an **OpenAI Images** provider (see [Image generation](#image-generation)).
- **Get a key**: <https://platform.openai.com/api-keys>
- **Cost**: pay-per-token, billed to your OpenAI account.

### Anthropic

- **What it provides**: Claude models (Claude Sonnet, Claude Opus, Claude Haiku).
- **Get a key**: <https://console.anthropic.com>
- **Cost**: pay-per-token, billed to your Anthropic account.

### Google Gemini

- **What it provides**: Gemini models (Gemini Pro, Gemini Flash).
- **Get a key**: <https://aistudio.google.com>
- **Cost**: free tier available; pay-per-token beyond it.

### OpenRouter

- **What it provides**: a single API that routes to many model families through one account. Useful when you want to try many models without separate keys.
- **Get a key**: <https://openrouter.ai>
- **Cost**: pay-per-token; rates depend on the underlying model OpenRouter routes to.

### DeepSeek

- **What it provides**: DeepSeek-Chat and DeepSeek-Reasoner models.
- **Get a key**: <https://platform.deepseek.com>
- **Cost**: pay-per-token, billed to your DeepSeek account.

> **Pro-tip**: OpenRouter and DeepSeek have an optional **Base URL** field. Leave it blank to use the official endpoint. Set it only if you need a proxy or a region-specific endpoint. OpenAI, Anthropic and Gemini always use their official endpoints.

## Remote servers

### Ollama

- **What it is**: a local-first server that runs open-source LLMs on your hardware. Install from <https://ollama.com>.
- **Default base URL**: `http://localhost:11434` (Ollama's default).
- **Setup**: pull a model with `ollama pull <model-name>` (e.g. `ollama pull qwen3:14b`), then configure Verzeta with the base URL.
- **LAN use**: point at another machine's IP if Ollama runs on a different host. Any URL this computer can reach works.

> **Pro-tip**: Use the **Test Connection** button in the Ollama setup sheet. It pings `<base-url>/api/tags`. A green check means the URL is reachable and Ollama is responding.

### llama.cpp (Remote)

- **What it is**: HTTP client for an externally-running `llama-server` (from the [llama.cpp](https://github.com/ggml-org/llama.cpp) project).
- **Default base URL**: `http://localhost:8080/v1`.
- **Auth**: typically none. If you put the server behind a reverse proxy that requires a bearer token, paste the token in the **Bearer Token** field.
- **Tool calling**: not all llama.cpp builds support tool calling. If yours does, tick **"My llama.cpp build supports tool calling"** in the setup sheet.

> **Note**: Unlike Ollama, llama.cpp does not expose a model-list endpoint we test against. Verify your server is reachable manually with `curl http://localhost:8080/v1/models` before relying on it.

## Local models

### llama.cpp (built-in engine)

- **What it is**: a llama.cpp engine that ships with the **Local AI** edition. It runs GGUF models in a separate helper program, `verzeta-inference`, so you do not need to install a server. In **Settings → Text Providers** it appears as **llama.cpp (Local)**; pick your `.gguf` file under **GGUF Model Path**.
- **Availability**: only in the **Local AI** edition (the download whose file name contains `localai`). In the Standard edition the wizard shows "Unavailable in this build" for it. To run local models on the Standard edition, use a local server such as Ollama or a self-hosted llama.cpp, as described under **Remote servers** above.
- **Memory cost**: about the size of the `.gguf` file. A 7B Q4_K_M model uses about 4 GiB.
- **GPU acceleration**: automatic. The Local AI edition uses Vulkan on your GPU when one is available and the CPU otherwise. Builds made from source may use CUDA or Metal instead.
- **Tool calling**: not supported. Agents that need tools should use another provider.
- **Reply length**: when **Max Tokens** is **Auto**, a reply is capped at 1,024 tokens. Set a value in Chat Settings for longer replies.
- **Heartbeats**: not available on this provider. Use a different provider for members with a heartbeat.

> **Note**: Because the model runs in its own process, a crash in the model does not close Verzeta Studio. Verzeta starts the engine again on the next request, and stops trying after three failed starts in a row until you restart the app.

## Custom OpenAI-compatible servers

In addition to the eight providers above, Verzeta also supports any server that speaks the OpenAI `/v1/chat/completions` plus `/v1/models` contract: vLLM, LM Studio, Jan, Llamafile, TabbyAPI, KoboldCpp, LocalAI, SGLang, text-generation-webui, or your own. Configure as many as you want; each one becomes a first-class provider that any agent can route through.

- **Where to configure**: **Settings → Text Providers → Custom OpenAI-Compatible Servers**, plus an opt-in entry on the first-run wizard.
- **What you need**: the server's base URL (almost always ending in `/v1`), an optional API key, and the capability flags (streaming, tool calling, vision) that match the server.
- **Test Connection** runs `GET /v1/models` against the URL and tells you what is wrong, if anything. For example, it reports when the base URL probably needs `/v1` at the end, when no models are loaded, or when the server requires an API key.
- **Multi-instance**: configure one server per LM Studio install, one per vLLM lab box, one per friend's GPU. Name them and route different members of a team to different servers in the same conversation.

See [12-self-hosted-servers.md](12-self-hosted-servers.md) for the full per-stack setup guide.

## How configured providers are available

A configured provider is available everywhere a provider is selectable:

- **Default provider** in **Settings → Defaults → Default Text Provider**: used by new conversations.
- **Per-conversation override** in a chat's **Chat Settings → Model**.
- **Per-member override** in **Chat Settings → Group Members**, using the configure button on a member's row: different agents can use different providers in one chat.
- **Background routing**: when a foreground provider has a matching background slot (Ollama, OpenAI, Anthropic, Gemini, OpenRouter, DeepSeek, llama.cpp Remote), heartbeat runs use that slot independently of foreground traffic.

> **Note**: The built-in llama.cpp engine is the only provider without a background slot, so heartbeat runs cannot use it.

## Switching providers

| Where                | How                                                                                            |
| -------------------- | ---------------------------------------------------------------------------------------------- |
| For the whole app    | **Settings → Defaults → Default Text Provider**, then pick a model in a chat's model selector |
| For one conversation | open the conversation, **Chat Settings → Model**                                    |
| For one team member  | **Chat Settings → Group Members** → configure button on the member (or **Folder Settings → Team Members** for a project) |

The change applies to the next message you send.

## Image generation

Some agents can create images with the **generate_image** tool, and every chat has a **Generate Image** button. Both are enabled only once you configure an image provider in **Settings → Providers → Image Generation** and set it active. If no provider is active, the button is hidden.

Add one or more of:

| Provider | What you need |
| --- | --- |
| **OpenAI Images** (DALL-E) | Your OpenAI API key. |
| **OpenAI-compatible chat-image** (OpenRouter) | An API key for the service. |
| **Automatic1111 / SD WebUI** | The base URL of your self-hosted Stable Diffusion server. |
| **Local CLI** | A local Stable Diffusion command-line tool on your machine. |

Set one provider active to use it. Most image models return an image only; a few also return a text caption, so if generation fails with an output-type error, pick the matching **Output** mode in the provider's setup sheet. A finished image appears in the chat and is also saved into your project's files under `images/`.

For image errors (a wrong base URL, a rejected key, or an image-only model asked for text), see [10-troubleshooting.md](10-troubleshooting.md).

## Web search

Agents that have the **search_web** tool look things up on the live web. You choose which search backend that tool uses in **Settings → Providers → Web Search**. Only one backend is active at a time, and the tool falls back to DuckDuckGo whenever the active one is unavailable or not yet configured.

| Backend | What you need |
| --- | --- |
| **DuckDuckGo** | Nothing. Works out of the box with no key or URL, and is the default. Best-effort, so it may be rate-limited under heavy use. |
| **Tavily**, **Exa**, **LangSearch** | An API key from that service. Paste the key, then set the backend active. |
| **SearXNG** (self-hosted) | The base URL of your own SearXNG instance. An API key is optional. |
| **Custom endpoint** (advanced) | The URL of a JSON search API you run, plus its HTTP method and auth header. |

A backend cannot be set active until its requirements are met (an API key for Tavily, Exa, or LangSearch; a base URL for SearXNG), and the page states inline what is still missing. Keys and URLs are stored locally, the same as other provider credentials.

> **Note**: Web search reaches the public internet by design. For a fully offline setup, remove **search_web** from an agent's tool whitelist (see [04-multi-agent-teams.md](04-multi-agent-teams.md)).

## Storage details

- API keys: stored locally in the application's settings (on Linux, `~/.config/Verzeta/verzeta-studio.conf`; on Windows, the registry under `HKEY_CURRENT_USER\Software\Verzeta\verzeta-studio`). Each key is obfuscated with a value derived from your machine. That stops a key being read at a glance, but it is not encryption: anyone who can read those settings on the same machine can recover the key, so protect your user account accordingly. Keys never leave your device except as bearer credentials to the matching provider.
- Base URLs: stored in the local database. Not secret.
- Provider toggles (e.g. "llama.cpp Remote supports tool calling"): stored locally as boolean preferences.

## Troubleshooting providers

If a provider is not working, see [10-troubleshooting.md](10-troubleshooting.md) for the most common issues:

- Ollama not connecting
- Cloud provider returns 401 or 403
- llama.cpp (Remote) returning empty replies
- A self-hosted OpenAI-compatible server that does not work
- API keys that stop working after moving to a new computer
