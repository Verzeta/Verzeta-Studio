<!--
SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
SPDX-License-Identifier: LGPL-3.0-or-later
-->

# Ollama via OpenAI-compat endpoint capture setup

## Server start

Ollama exposes `/v1/chat/completions` automatically when running.

```bash
ollama serve              # starts the server
ollama pull qwen2.5:7b    # one-time model pull
ollama run qwen2.5:7b ""  # warm-up; can be skipped if already loaded
```

Confirm:

```bash
curl -s http://localhost:11434/v1/models | jq .
```

## Capture commands

```bash
cd tests/integration/fixtures/openai-compat/ollama-openai-compat

# 1. Model list.  Ollama returns every locally-pulled GGUF.
curl -s http://localhost:11434/v1/models > models-list.json

# 2. Non-tool streaming turn.
curl -sN -H 'Content-Type: application/json' \
    -d '{
        "model": "qwen2.5:7b",
        "stream": true,
        "messages": [
            {"role": "user", "content": "Reply with the word OK."}
        ]
    }' \
    http://localhost:11434/v1/chat/completions \
    > chat-streaming-stop.sse

# 3. Tool-call streaming turn.
curl -sN -H 'Content-Type: application/json' \
    -d '{
        "model": "qwen2.5:7b",
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
    http://localhost:11434/v1/chat/completions \
    > chat-streaming-tool-call.sse
```

## Version

Record:

```bash
ollama --version
```

> Ollama version captured against: **FILL IN**

## Gotchas

- Ollama supports `/v1/chat/completions` from v0.5 onwards. Earlier
  versions only had the native `/api/chat` endpoint, which is
  covered by the dedicated `OllamaProvider` rather than this
  OpenAI-compat path.
- Model ids use the `name:tag` form (`qwen2.5:7b`, not `qwen2.5-7b`).
- The OpenAI-compat path is a thin shim over Ollama's native API;
  tool calling here goes through the same model-side code path as
  the native OllamaProvider. A regression in OUR
  OpenAICompatProvider's tool-call parser would not break the
  native OllamaProvider. Both are pinned by their own fixture
  sets.
