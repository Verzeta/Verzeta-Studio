<!--
SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
SPDX-License-Identifier: LGPL-3.0-or-later
-->

# llama.cpp server (`llama-server`) capture setup

## Server start

```bash
# Replace the model path with whatever you have locally.  The model
# MUST support tool calling for the tool-call fixture to be useful;
# Llama-3.1-Instruct, Qwen-2.5-Instruct, Hermes-2-Pro all work.
llama-server \
    --model /path/to/Qwen2.5-7B-Instruct-Q5_K_M.gguf \
    --port 8080 \
    --jinja \
    --chat-template chatml \
    --ctx-size 8192 \
    -ngl 999
```

The `--jinja` flag is required for tool-call template rendering.
The `--chat-template` flag selects which chat-template the server
applies; some `chatml` variants advertise tool support.

Confirm:

```bash
curl -s http://localhost:8080/v1/models | jq .
```

## Capture commands

```bash
cd tests/integration/fixtures/openai-compat/llamacpp-server

# 1. Model list.
curl -s http://localhost:8080/v1/models > models-list.json

# 2. Non-tool streaming turn.
curl -sN -H 'Content-Type: application/json' \
    -d '{
        "model": "default",
        "stream": true,
        "messages": [
            {"role": "user", "content": "Reply with the word OK."}
        ]
    }' \
    http://localhost:8080/v1/chat/completions \
    > chat-streaming-stop.sse

# 3. Tool-call streaming turn.
curl -sN -H 'Content-Type: application/json' \
    -d '{
        "model": "default",
        "stream": true,
        "tools": [{
            "type": "function",
            "function": {
                "name": "get_weather",
                "description": "Get the current weather for a location.",
                "parameters": {
                    "type": "object",
                    "properties": {"location": {"type": "string"}},
                    "required": ["location"]
                }
            }
        }],
        "tool_choice": "auto",
        "messages": [
            {"role": "user", "content": "What is the weather in Paris?"}
        ]
    }' \
    http://localhost:8080/v1/chat/completions \
    > chat-streaming-tool-call.sse

# 4. Empty-API-key dispatch regression.  Send the same non-tool turn
#    with NO Authorization header and confirm the capture is
#    identical.  This pin uses the existing chat-streaming-stop.sse
#    as fixture data; no separate file needed.
```

## Build version

Record the `llama-server --version` output here:

> llama.cpp build captured against: **FILL IN**
> `--jinja` enabled: **FILL IN** (`yes` / `no`)
> Chat template: **FILL IN** (e.g. `chatml`, `llama3`, `qwen`)

## Gotchas

- The server exposes only ONE model: the GGUF passed on the
  command line. `/v1/models` returns it under the id `"default"`.
- Some upstream builds disable Jinja templating at compile time;
  without `--jinja` the `tools` field is silently ignored and the
  capture is identical to the non-tool case. Confirm by inspecting
  the SSE. A real tool-call capture contains a `tool_calls` array
  inside `delta`.
