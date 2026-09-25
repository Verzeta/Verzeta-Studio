<!--
SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
SPDX-License-Identifier: LGPL-3.0-or-later
-->

# LM Studio capture setup

## Server start

1. Open LM Studio.
2. Developer tab → "Start Server" → confirm the toggle is green.
3. Chat tab → load a tool-call-capable model (e.g. Llama-3.1-8B-Instruct-Q4_K_M, Qwen-2.5-7B-Instruct).
4. Confirm:
   ```bash
   curl -s http://localhost:1234/v1/models | jq .
   ```

## Capture commands

```bash
cd tests/integration/fixtures/openai-compat/lm-studio

# 1. Model list with one model loaded.
curl -s http://localhost:1234/v1/models > models-list.json

# 1b. (Optional) Model list with NO model loaded. Unload the model
#     in the Chat tab first, then capture; this pins the friend's
#     "empty model list" failure mode against a real response.
curl -s http://localhost:1234/v1/models > models-list-empty.json

# 2. Non-tool streaming turn.
curl -sN -H 'Content-Type: application/json' \
    -d '{
        "model": "llama-3.1-8b-instruct",
        "stream": true,
        "messages": [
            {"role": "user", "content": "Reply with the word OK."}
        ]
    }' \
    http://localhost:1234/v1/chat/completions \
    > chat-streaming-stop.sse

# 3. Tool-call streaming turn. Only works on a model whose chat
#    template advertises tool support.  Llama-3.1-Instruct does;
#    older Llama-2 quants do not.
curl -sN -H 'Content-Type: application/json' \
    -d '{
        "model": "llama-3.1-8b-instruct",
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
    http://localhost:1234/v1/chat/completions \
    > chat-streaming-tool-call.sse
```

## Gotchas

- The model id LM Studio returns from `/v1/models` may be the GGUF
  filename (`llama-3.1-8b-instruct-q4_k_m.gguf`) rather than a clean
  name. Use whatever `models-list.json` reports.
- LM Studio v0.3.x and later support tool calling; earlier versions
  silently ignore the `tools` field and return a plain text response.
  Record the version in this file:

  > LM Studio version captured against: **FILL IN**
