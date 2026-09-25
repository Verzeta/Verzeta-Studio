<!--
SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
SPDX-License-Identifier: LGPL-3.0-or-later
-->

# Per-stack fixture replay tests for OpenAI-API-compatible servers

This directory hosts captured real-server responses from each Tier 1
self-hosted LLM stack we aim to support well. The replay
harness in [test-openai-compat-fixture-replay.cpp](../../test-openai-compat-fixture-replay.cpp)
walks each subdirectory at test time, mounts the captured files as
mock-server responses, runs them through `OpenAICompatProvider`'s
real SSE / JSON / tool-call parser, and pins the parsed shape with
assertions.

The point: synthetic test data does not catch parser divergence from
real-server output. vLLM's `--tool-call-parser hermes` emits a chunk
shape that differs subtly from the `llama3_json` parser; LM Studio
v0.3 returns a slightly different `/v1/models` shape than v0.2; Jan
omits the `system_fingerprint` field where vLLM includes one. These
are the kinds of bugs only real fixtures catch, and they are exactly
the kind of bug a user would file as "vLLM tool calling doesn't work"
when in fact our parser silently dropped the chunk.

## Stacks covered

| Slug                   | Stack                                          | Default base URL            |
| ---------------------- | ---------------------------------------------- | --------------------------- |
| `vllm`                 | vLLM                                           | `http://localhost:8000/v1`  |
| `lm-studio`            | LM Studio                                      | `http://localhost:1234/v1`  |
| `llamacpp-server`      | llama.cpp server (`llama-server` binary)       | `http://localhost:8080/v1`  |
| `ollama-openai-compat` | Ollama via its `/v1/chat/completions` endpoint | `http://localhost:11434/v1` |

At 1.0.0 only `vllm/` holds captured fixtures. The other three
directories hold a `SETUP.md` with the capture steps, and the harness
skips them until fixtures are added.

## Fixture file naming

Each stack subdirectory holds JSON and SSE files named by scenario:

| Filename                             | Format | Purpose                                                                             |
| ------------------------------------ | ------ | ----------------------------------------------------------------------------------- |
| `models-list.json`                   | JSON   | Captured `GET /v1/models` response                                                  |
| `chat-streaming-stop.sse`            | SSE    | Captured `POST /v1/chat/completions` non-tool turn                                  |
| `chat-streaming-tool-call.sse`       | SSE    | Captured turn that emits a single tool call                                         |
| `chat-streaming-tool-multi-turn.sse` | SSE    | Captured continuation after submitting a tool result                                |
| `chat-streaming-empty.sse`           | SSE    | Captured turn with no `finish_reason` (the empty-stream safety-net regression case) |

Any subset is acceptable per stack. The replay harness covers
whatever it finds and reports skipped scenarios cleanly. The goal for
each stack is at least one `models-list.json` plus one of the
streaming files.

## Capture procedure (per stack)

See the SETUP.md in each stack subdirectory for the exact command
lines. The general pattern:

1. Start the server with the smallest tool-call-capable model you can
   load (typical: Qwen-2.5-7B-Instruct, Llama-3.1-8B-Instruct,
   Mistral-7B-Instruct, or Phi-3-Mini-Instruct). For vLLM, pass
   `--enable-auto-tool-choice --tool-call-parser <parser>`.
2. Capture `GET /v1/models`:
   ```bash
   curl -s http://<server>/v1/models > models-list.json
   ```
3. Capture a non-tool streaming turn:
   ```bash
   curl -sN -H 'Content-Type: application/json' \
       -d '{"model":"<model-id>","stream":true,
            "messages":[{"role":"user","content":"Reply with the word OK."}]}' \
       http://<server>/v1/chat/completions \
       > chat-streaming-stop.sse
   ```
4. Capture a tool-call streaming turn:
   ```bash
   curl -sN -H 'Content-Type: application/json' \
       -d @tool-call-request.json \
       http://<server>/v1/chat/completions \
       > chat-streaming-tool-call.sse
   ```
   where `tool-call-request.json` contains a request with one tool
   defined (a minimal `get_weather` schema) and a user message that
   forces tool invocation ("What is the weather in Paris?").
5. **Strip any per-run identifiers** that would cause the fixture to
   fail deterministic-replay assertions: `id` strings, ISO
   timestamps, `system_fingerprint`, `request_id`. The harness
   normalises these at parse time but stripping at capture time keeps
   the fixture file diff-friendly.

## Stack-specific gotchas

### vLLM

- Tool calling requires the server be started with
  `--enable-auto-tool-choice --tool-call-parser <parser>`. Available
  parsers: `hermes`, `mistral`, `llama3_json`, `pythonic`, `granite`,
  `qwen2_5`. Capture one fixture per parser if you can, because each emits
  a different `tool_calls` chunk shape.
- vLLM emits `system_fingerprint` on most responses; ours does not
  read it, so stripping is fine.

### LM Studio

- Server mode must be explicitly enabled via the Developer tab.
- The loaded model must be selected from the Chat tab before
  `/v1/models` returns a populated list. Capture the
  no-model-loaded case too if possible so we can pin the
  `no_models_loaded` verdict against a real LM Studio response, not a
  synthetic one.
- LM Studio's tool-call support landed in v0.3.x and depends on the
  loaded model's chat template advertising tool support.

### llama.cpp server

- Tool calling requires the server build to ship with `--jinja`
  enabled AND the loaded GGUF to carry a tool-call-capable chat
  template (Hermes-2-Pro, Qwen-2.5, Llama-3.1-Instruct, etc.). Most
  pre-built `llama-server` binaries from upstream releases enable
  this; some packaged builds disable it.
- Auth is off by default, so capture with no Authorization header.

### Ollama OpenAI-compat

- Ollama's `/v1/chat/completions` endpoint became fully tool-call-
  compatible in v0.5. Earlier versions emit a slightly different
  envelope. Capture the version with `ollama --version` and write
  it into `SETUP.md`.

## When fixtures land

The replay harness runs every fixture on every CI build. A parser
regression that breaks a Tier 1 stack will surface as a failing
assertion in `test-openai-compat-fixture-replay` BEFORE a user files
"vLLM tool calling broke after Verzeta v1.0.5."
