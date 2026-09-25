<!--
SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
SPDX-License-Identifier: LGPL-3.0-or-later
-->

# Self-hosted OpenAI-compatible servers

Verzeta ships with a **Custom OpenAI-Compatible Server** provider you can
configure once per server and route any agent through. Anything that
respects the OpenAI `/v1/chat/completions` plus `/v1/models` contract works:
vLLM, LM Studio, Jan, Llamafile, TabbyAPI, KoboldCpp, LocalAI, SGLang,
text-generation-webui, or your own server.

You can configure **multiple** at once: an LM Studio at home, a vLLM on a
lab machine, a TabbyAPI on a friend's GPU box. Each becomes a first-class
provider in the dropdowns, and a single multi-agent team can route each
member to a different server.

## Where to find it

- **Settings → Text Providers → Custom OpenAI-Compatible Servers**:
  list, add, edit, delete.
- **First-run wizard**: a "Custom OpenAI-Compatible Server" row appears
  alongside the cloud and local providers. Click _Configure_ to add
  one during first run. You can skip it.

## Adding a server

1. **Display name (Required)**: what you want to call this server in the dropdown
   ("LM Studio at home", "vLLM lab box"). Pick something you will
   recognise; you can rename it later without disrupting anything.
2. **Base URL (Required)**: the server's OpenAI-compatible root. **Include `/v1`
   at the end if your server expects it**, for example
   `http://localhost:1234/v1` for LM Studio, not
   `http://localhost:1234`. Most stacks need the `/v1`; getting it
   wrong is the most common cause of "the model list is empty".
3. **Server requires an API key**: leave off for local servers that do not
   need authentication. Turn it on when you have fronted the server
   with a reverse proxy that wants a bearer token, or when the server
   itself requires one.
4. **API key**: only meaningful when **Server requires an API key** is on. Stored
   the same way as the other provider keys (see
   [Storage details](02-providers.md#storage-details)).
5. **Capability flags**: streaming, tool calling and vision (labelled
   **Server supports SSE streaming**, **Server supports tool calling**,
   and **Server accepts image INPUT (vision)**). Set them to what your
   server supports. Default is streaming on,
   tool calling on, vision off. Agents that need tool calling are
   routed only to servers with the flag on.

Then click **Test Connection** before **Add** / **Save**. If the test
passes Verzeta caches the model list immediately so the dropdown is
populated the moment you save.

## Test Connection verdicts

The Test Connection button reports one of these results:

| Verdict                         | What it means                                                     | What to do                                                                                                  |
| ------------------------------- | ----------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------- |
| **OK**                          | The server is reachable and at least one model is loaded.         | Click Add / Save.                                                                                           |
| **No models loaded**            | The server is reachable, but `/v1/models` returned an empty list. | Load a model on the server first. LM Studio: Chat tab → load a model. vLLM: pass `--model <id>` at startup. |
| **/v1/models not found**        | The server is reachable, but the URL points at the wrong path.    | Add `/v1` at the end of the base URL.                                                                       |
| **Server requires an API key**  | The server returned 401 and no key was sent.                      | Turn on **Server requires an API key** and enter your key.                                                  |
| **Server rejected the API key** | The server returned 401 even with the key.                        | Re-check the key value.                                                                                     |
| **Server error (HTTP 5xx)**     | The server is reachable but reported an internal error.           | Check the server's own logs.                                                                                |
| **Cannot reach the server**     | TCP / DNS / refused.                                              | Check the URL is correct, the server is running, and the port is open.                                      |
| **Unexpected HTTP status**      | The server answered with a status Verzeta does not expect.        | Check the URL and the server's logs.                                                                        |

## Pre-fill catalogue

The setup sheet's "Start from a known stack…" dropdown seeds the URL
and capability flags for nine popular stacks. Picking one fills the
form; you can edit any field afterward.

| Stack                 | Default URL                 | Streaming | Tools | Vision |
| --------------------- | --------------------------- | :-------: | :---: | :----: |
| vLLM                  | `http://localhost:8000/v1`  |     ✓     |   ✓   |   ✗    |
| LM Studio             | `http://localhost:1234/v1`  |     ✓     |   ✓   |   ✗    |
| Jan                   | `http://localhost:1337/v1`  |     ✓     |   ✓   |   ✗    |
| Llamafile             | `http://localhost:8080/v1`  |     ✓     |   ✗   |   ✗    |
| TabbyAPI              | `http://localhost:5000/v1`  |     ✓     |   ✓   |   ✗    |
| KoboldCpp             | `http://localhost:5001/v1`  |     ✓     |   ✗   |   ✗    |
| LocalAI               | `http://localhost:8080/v1`  |     ✓     |   ✓   |   ✗    |
| text-generation-webui | `http://localhost:5000/v1`  |     ✓     |   ✗   |   ✗    |
| SGLang                | `http://localhost:30000/v1` |     ✓     |   ✓   |   ✗    |

Vision is off in every pre-fill because most local stacks do not
report vision support reliably. Turn it on per server if you know
yours does.

## Stack notes

### vLLM

Tool calling requires the server be started with
`--enable-auto-tool-choice --tool-call-parser <parser>`. Available
parsers include `hermes`, `mistral`, `llama3_json`, `pythonic`,
`granite`, and `qwen2_5`. Pick the parser that matches the model you
have loaded; an incompatible parser drops the `tool_calls`
chunk and the agent thinks the tool was never invoked.

### LM Studio

Server mode must be explicitly enabled in the Developer tab. Load a
tool-call-capable model from the Chat tab. Current Llama-3.1-Instruct
and Qwen-2.5-Instruct quants advertise tool support; older Llama-2
quants do not.

### Jan

Server mode is in Settings → Local API Server in recent builds. The
defaults match the pre-fill above.

### llama.cpp server (`llama-server`)

The built-in **llama.cpp (Remote)** provider already covers this
stack and has a **My llama.cpp build supports tool calling** toggle of its own; use
that one for plain `llama-server` builds. Reach for the Custom
OpenAI-Compatible Server provider when you want to register multiple
llama.cpp servers on different hosts, or when you need to set
capability flags per server.

Tool calling on llama.cpp server requires the server build ship with
`--jinja` enabled AND the loaded GGUF carry a tool-call-capable chat
template (Hermes-2-Pro, Qwen-2.5-Instruct, Llama-3.1-Instruct).

### Llamafile

Tool calling is not enabled by default in the Llamafile single-binary
build; leave the flag off. Streaming works.

### TabbyAPI

Tool calling support depends on the loaded model and the
exllamav2 build. Test against your specific configuration before
relying on it.

### KoboldCpp

The OpenAI-compat layer in KoboldCpp does not pass the `tools` field
through to the model's chat template, so leave the tool-calling flag
off. Streaming and plain chat work.

### LocalAI

Multi-backend; the capability of any given model depends on which
backend serves it (llama.cpp / vllm / exllamav2 / etc.).

### SGLang

Tool calling lands via SGLang's `--tool-call-parser` flag, similar to
vLLM.

### text-generation-webui (Oobabooga)

The OpenAI extension must be explicitly enabled in
`settings.yaml → openai`. Tool calling support is uneven across
backends.

## What "capability flag" means here

Each flag is a contract you are advertising to Verzeta, not a probe
of the server. When you turn **Tool calling** on for a server,
Verzeta will route agents that depend on tools to that server. If
the server (or the loaded model) does not support tool
calling, the agent will end up looping or producing a plain-text
response; the failure is at the server's end. Turn the flag on
only when you know your server-and-model combination handles the
shape correctly.

## Multi-instance routing: the team angle

Members of one team can use different servers. In a project or group
chat:

- Set the conversation's primary provider to your fastest server for
  the coordinator.
- In **Chat Settings → Group Members** (or **Folder Settings → Team
  Members** for a project), give each member a per-member
  provider override pointing at the server that fits its job. A
  reviewer member can sit on a tool-call-capable vLLM build; a
  writer member can sit on an LM Studio server; a
  researcher member can sit on Claude in the cloud.

The cascade preserves per-member routing on every turn: each member's
turn dispatches to its assigned provider regardless of what the
member before it used.

## Thinking mode and your server

The per-conversation **Thinking** toggle is honoured for
self-hosted servers as well as cloud providers. Verzeta passes the
toggle to the server as a `chat_template_kwargs.enable_thinking`
field on every chat request. Models whose chat template reads the
kwarg (qwen3 family, DeepSeek-R1, gpt-oss-thinking, etc.) respect
the toggle; templates that do not reference it ignore the
field. You do not need to configure anything on the server.

When the model emits reasoning content (either through a separate
channel like vLLM's `delta.reasoning` or llama.cpp server's `--jinja`
thinking, or inline as `<think>...</think>` blocks in content) it
lands in the collapsed Reasoning disclosure inside the assistant
bubble. Display-only: reasoning is never re-sent on the next turn,
so the per-turn context cost stays bounded. See **Thinking mode**
in [03-conversations.md](03-conversations.md) for the full per-provider
behaviour.

## On Android

The Android client lists every configured custom server in the
provider dropdowns and lets you route agents through them. The
client is **read-only** for the catalogue itself: adding, editing,
and deleting custom servers happens on the desktop.

## Privacy

Custom servers are configured locally and live only in your local
SQLite database. Their API keys are stored the same way as the
cloud-provider keys (see [Storage details](02-providers.md#storage-details)). Nothing
leaves your machine except the chat completion requests, which go
directly to the server you configured. Nothing passes through any
Verzeta service.
