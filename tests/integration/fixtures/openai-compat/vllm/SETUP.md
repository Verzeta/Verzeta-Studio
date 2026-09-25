<!--
SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
SPDX-License-Identifier: LGPL-3.0-or-later
-->

# vLLM capture setup

## Server start

```bash
vllm serve Qwen/Qwen2.5-7B-Instruct \
    --port 8000 \
    --enable-auto-tool-choice \
    --tool-call-parser hermes
```

Confirm the server is up:

```bash
curl -s http://localhost:8000/v1/models | jq .
```

## Capture commands

```bash
cd tests/integration/fixtures/openai-compat/vllm

# 1. Model list.
curl -s http://localhost:8000/v1/models > models-list.json

# 2. Non-tool streaming turn.
curl -sN -H 'Content-Type: application/json' \
    -d '{
        "model": "Qwen/Qwen2.5-7B-Instruct",
        "stream": true,
        "messages": [
            {"role": "user", "content": "Reply with the word OK."}
        ]
    }' \
    http://localhost:8000/v1/chat/completions \
    > chat-streaming-stop.sse

# 3. Tool-call streaming turn.
curl -sN -H 'Content-Type: application/json' \
    -d '{
        "model": "Qwen/Qwen2.5-7B-Instruct",
        "stream": true,
        "tools": [{
            "type": "function",
            "function": {
                "name": "get_weather",
                "description": "Get the current weather for a location.",
                "parameters": {
                    "type": "object",
                    "properties": {
                        "location": {"type": "string"}
                    },
                    "required": ["location"]
                }
            }
        }],
        "tool_choice": "auto",
        "messages": [
            {"role": "user", "content": "What is the weather in Paris?"}
        ]
    }' \
    http://localhost:8000/v1/chat/completions \
    > chat-streaming-tool-call.sse
```

## Parser variants

Capture one tool-call fixture per parser if you can, because they emit
materially different `tool_calls` chunk shapes:

| Parser        | Filename suffix                            |
| ------------- | ------------------------------------------ |
| `hermes`      | `chat-streaming-tool-call.sse`             |
| `llama3_json` | `chat-streaming-tool-call-llama3_json.sse` |
| `mistral`     | `chat-streaming-tool-call-mistral.sse`     |
| `pythonic`    | `chat-streaming-tool-call-pythonic.sse`    |
| `granite`     | `chat-streaming-tool-call-granite.sse`     |
| `qwen2_5`     | `chat-streaming-tool-call-qwen2_5.sse`     |

To capture each, restart vLLM with `--tool-call-parser <name>` and
re-run the tool-call command above. The `hermes` capture is the
mandatory one for v1.0.0; others are nice-to-have.

## Verifications

After capture:

```bash
# Strip blank lines + check format.
head -10 chat-streaming-stop.sse
# Should start with: data: {"id":"chatcmpl-...","choices":[...
```

If `[DONE]` is missing at the end of the file, vLLM closed the
stream without emitting the terminator (rare). Add it manually:
`echo 'data: [DONE]' >> chat-streaming-stop.sse`.
