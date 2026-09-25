# Troubleshooting

This page covers the most common issues users hit and how to resolve them. For questions about features and intended behaviour, see [11-faq.md](11-faq.md).

Most errors from a model provider appear in a red banner at the top of the window. Errors from tools appear in the chat and in the **Tool Log** in the chat header. When this page quotes a message, `@Name` stands for the agent's alias and `X` for a program or file name.

## 1. Ollama is configured but does not reply / shows a connection error

**Symptoms**

- The provider chip on the home screen says Ollama is active, but messages produce no reply.
- An error banner appears at the top of the window, for example "Connection refused", "No LLM provider is active. Choose a provider and model first.", or "HTTP 404: ..." followed by Ollama's own message.
- In the Ollama setup sheet, **Test Connection** shows a red ✗ or a timeout.

**Common causes and fixes**

- **Ollama is not running on your machine.**
  - Open a terminal and run: `ollama list`
  - If you see "Error: could not connect to ollama app, is it running?", start Ollama. On most Linux distros: `systemctl --user start ollama` or launch the Ollama app.

- **Ollama is running but no model is pulled.**
  - In a terminal: `ollama list`. If the list is empty, pull a model: `ollama pull qwen3:8b` (or any model name).
  - In Verzeta, open a chat and check that the model selector lists your model.

- **The error says the model was not found.** The model name in Verzeta does not match a pulled model. Run `ollama list` and pick one of those names.

- **The error says the model does not support tools.** Tools are on by default. Pick a model that supports tool calling, or turn off **Tools** in the message bar for this chat.

- **The error says the model does not support thinking.** Turn off **Thinking** in the message bar, or pick a reasoning model.

- **The base URL is wrong.**
  - Default is `http://localhost:11434`. Verify this in **Settings → Text Providers → Ollama**.
  - If Ollama runs on a different port (set via `OLLAMA_HOST=…`), update the base URL accordingly.
  - Use `http://localhost:11434` without a trailing `/`.

- **Firewall is blocking the connection** (especially when Ollama is on a different machine).
  - Try `curl http://<ollama-host>:11434/api/tags` from the Verzeta machine. If this fails, fix network reachability first.

- **A long first reply fails after two minutes.** A streaming request that receives no data for 120 seconds is cancelled. Large models on a CPU can take that long to read a long conversation. Use a smaller model, a smaller **Context Window**, or a GPU.

> **Pro-tip**: The Ollama setup sheet's **Test Connection** button does the diagnostic call for you. Use it before sending the first chat.

## 2. A cloud provider returns 401, 403, or "Invalid API key"

**Symptoms**

- Messages to OpenAI / Anthropic / Gemini / OpenRouter / DeepSeek error out immediately.
- The error banner shows "HTTP 401" or "HTTP 403" followed by the provider's message, for example "Invalid API key".

**Common causes and fixes**

- **The API key is not stored or is wrong.**
  - Open **Settings → Text Providers**, find the provider card, and click **Edit** (or **Set up**).
  - Retype the API key. If the field shows "Stored. Type to replace, or leave blank to keep", typing a new value replaces the old one.
  - Click **Save**.

- **You changed the API key on the provider's website, but Verzeta still has the old one.**
  - Same fix: enter the new key in the setup sheet and Save.

- **You moved to a new computer or copied your settings.** API keys only work on the computer where you entered them. See [section 14](#14-api-keys-are-missing-or-rejected-after-moving-to-a-new-computer).

- **The provider account has run out of credit or hit a rate limit.**
  - Check your billing on the provider's dashboard (for example <https://platform.openai.com/usage>). Verzeta retries a rate-limited request (HTTP 429) three times, waiting 1, 2 and 4 seconds. If it still fails, or the provider returns 402 (out of credit), the provider's message appears in the error banner.

- **The model name is wrong for this provider.**
  - For example, asking OpenAI for `claude-...` returns a 404. Pick a model from the provider's own model list in the chat's model selector.

- **A regional or proxy endpoint is required.**
  - OpenRouter and DeepSeek have a **Base URL (optional)** field in their setup sheet. OpenAI, Anthropic and Gemini always use their official endpoints; for a proxy in front of them, add it as a custom OpenAI-compatible server (see [12-self-hosted-servers.md](12-self-hosted-servers.md)).

> **Note**: API keys are stored in the settings file on this computer and are sent only to the provider they belong to. If a key was working and suddenly stops, check the provider's status page.

## 3. An agent loops on tool calls and never produces a final reply

**Symptoms**

- A member keeps calling tools without finishing its reply.
- The chat shows "⏸ @Name reached the limit of 25 tool steps per turn and paused, so some work may be unfinished. Say "continue" to let it keep going, or raise "Tool steps per turn" in Chat Settings."
- The chat shows "⚠ @Name had 5 tool calls fail in a row, so the run was stopped to avoid a loop. Check the errors above, then give direction or say "try again"."

**Common causes and fixes**

- **The tool keeps returning an error.** Open the **Tool Log** in the chat header, or the **Activity Log** for the conversation (see [09-activity-timeline.md](09-activity-timeline.md)), and read the error. Fix the cause (a wrong file path, a program that is not on the allow-list, a missing API key), then tell the agent to try again. After 5 failed tool calls in a row the run stops on its own.
- **The agent needs a tool it does not have.** Open **Chat Settings → Group Members**, press the configure button on the member, and check its **Tool Whitelist**. For a project member, use **Folder Settings → Team Members**.
- **The task needs more steps than one turn allows.** Each turn may chain up to 25 tool calls by default. Say "continue" to let the agent carry on, or change **Tool steps per turn** in **Chat Settings → Tools**. The setting applies to every conversation. 0 means unlimited; the 5-failure stop still applies.
- **The model is not suited to the task.**
  - Smaller local models sometimes loop on multi-step plans. Try a larger model or a cloud model for the same prompt.

> **Pro-tip**: To stop a reply at any time, click **Stop** (the Send button turns into Stop while a reply streams). The current request is cancelled and the cascade does not advance. **Stop Task** in the message bar stops every active task in the chat.

## 4. Android pairing fails or the Android client cannot connect

**Symptoms**

- The Android app reports that pairing failed, the connection was refused, or the certificate does not match. The desktop rejects a bad code with "invalid or expired pairing code".

**Common causes and fixes**

- **The pairing code expired or was already used.** Codes work once and expire after 5 minutes. Generate a new code (**Settings → Remote Access → Manage Remote Access → Generate pair code**) and try again.

- **The desktop and the phone are not on the same network.** Check that both devices have addresses on the same subnet, or that the desktop's address is reachable from the phone through your router or VPN. From another computer you can test the port with `nc -vz <desktop-ip> 9180`.

- **TLS is enabled and the Android client does not have the right fingerprint.** When you pair, the Android app pins the desktop's TLS certificate by its SHA-256 fingerprint. If you press **Regenerate certificate** on the desktop, the fingerprint changes and the pinned value no longer matches. Copy the new fingerprint from **Settings → Remote Access → Manage Remote Access**, edit the host on Android, paste it, and pair again.

- **The address on the phone is incomplete.** The address must include the port and end in `/ws`, for example `ws://192.168.1.42:9180/ws`, and must start with `wss://` when TLS is on.

- **The Remote Access dialog says "Failed to start: check the log".** Open the log (see [Where to find more detail](#where-to-find-more-detail)). Common reasons: the `verzeta-remote` program is missing from the folder that contains `verzeta-studio`, or TLS is on but the `openssl` command is not installed (Verzeta runs `openssl` to create its certificate).

- **The server stops again right after you turn it on.** Another program may already use the port. Pick a different port in **Manage Remote Access**, or find what uses it with `ss -tlnp | grep :9180` on Linux.

- **You changed the TLS switch and nothing happened.** The change applies the next time the server starts. Turn the server off and on in **Manage Remote Access**.

- **A device that used to work is refused.** The desktop answers "invalid or revoked token": the device was revoked, or the host's pairing database was deleted. Pair it again.

- **The remote server is not running.** In **Settings → Remote Access → Manage Remote Access**, turn the server switch off and on, and check that the address under **Clients can reach this host at** is current. For detail, start Verzeta Studio with `QT_LOGGING_RULES="verzeta.remote*=true"` and check the main log file.

- **A firewall is blocking the WebSocket port.** Default is 9180 (the same port with or without TLS). Open the port in your firewall, or pick a different port in **Manage Remote Access**.

> **Warning**: When TLS is enabled and the fingerprint mismatches, the Android client refuses the connection by design. Do not click past the warning if it ever appears: it means someone might be impersonating the desktop. Verify the fingerprint on the desktop first.

## 5. Qwen 3.5 / 3.6 group-chat replies are truncated mid-sentence

**Symptoms**

- Past roughly 10-25 messages in a Qwen 3.5 or 3.6 group chat, agent replies cut off mid-sentence or mid-list (e.g. "I will help with" with no continuation).
- Pressing **Retry last response** sometimes gives a full reply, sometimes another truncated one.
- The same conversation, switched to Gemma or any non-Qwen model, works fine.

**Root cause**

Qwen 3.5/3.6 models ship with aggressive sampling defaults in their bundled Ollama modelfile: `presence_penalty = 1.5` and `repeat_penalty = 1.1` on top of loose default sampling. In a long group-chat context those penalties push the model to end its reply after only a sentence or two, which looks like truncation. (The same conversation works on Gemma, and setting the presence penalty to 0 on Qwen fixes it.)

**The fix: automatic**

Verzeta Studio ships a per-model sampling profile that is applied automatically whenever a conversation uses Qwen 3.5/3.6 on Ollama:

```
temperature 0.6 · top_k 10 · top_p 0.5 · repeat_penalty 1.03
presence_penalty 0 · frequency_penalty 0
```

These values were chosen by testing them on real conversations. They stop the early cut-offs while keeping enough repetition penalty (`repeat_penalty 1.03`) that agents do not loop. You do not need to do anything: new and existing Qwen conversations get the profile by default.

**Adjusting or opting out**

Open **Chat Settings** and look for **"Use app-recommended sampling"**:

- **Checked (default):** the profile above overrides the advanced sampling sliders at request time.
- **Unchecked:** your own slider values win; the profile only fills in values you left on "Auto".

If you opt out and truncation returns, tick the box again. If you want to experiment, keep `repeat_penalty` above 1.0 (at 1.0 agents repeat themselves) and below 1.04 (at 1.04 and above the cut-offs return). Any `presence_penalty` above 0 combined with a repeat penalty also brings truncation back.

**Still seeing truncation?**

1. Confirm the conversation is using Ollama with a `qwen3.5`/`qwen3.6` model (the checkbox only appears when a profile exists for the active provider and model).
2. Confirm the box is checked in *this* conversation: the setting is per conversation.
3. Check your Ollama server's context length (`OLLAMA_CONTEXT_LENGTH`); a very small value clamps the prompt on the server, which is a different problem with similar symptoms (see section 1).

## 6. Image generation produces nothing (no image, no error)

**Symptoms**

- You ask an agent to generate an image; it replies "the image will appear shortly," but no image ever arrives.
- Your image provider's own dashboard shows no request.

**Most common cause: the Base URL includes the endpoint path**

The **Base URL** field for an HTTP image provider expects the API *base*, for example:

```
https://openrouter.ai/api/v1
```

Verzeta appends the endpoint path itself (`/chat/completions` for OpenRouter-style chat-image providers, `/images/generations` for OpenAI-style, `/sdapi/v1/txt2img` for Automatic1111). Pasting the full documented endpoint, such as `https://openrouter.ai/api/v1/chat/completions`, also works.

**Failures are shown in the chat**

Image generation runs in the background after the tool call returns. A failure can be a misconfigured URL, a missing or rejected API key, a model that does not support image output, or a network error. Any of them is written into the conversation as a message beginning with **"⚠ Image generation failed:"** followed by the provider's reason. If you do not see an image, look for that message; it says what went wrong.

When an image succeeds, it appears in the chat **and** is saved into your project's files under `images/` (the message names the file, e.g. *Saved to project files: images/hero-banner-1a2b3c4d.png*), so it shows up alongside the rest of the team's artifacts.

**"No endpoints found that support the requested output modalities: image, text"**

Most dedicated image-generation models (e.g. Flux, grok-imagine) output **only an image** and have no text output. Asking such a model for *both* image and text fails with this error. In the provider's setup sheet set **Output** to **"Image only"** (the default). Choose **"Image + text"** only for dual-output models like Gemini that also return a text caption.

**Editing a generated image**

Click a generated image to open the preview. When your active provider is a chat-image provider (one whose model accepts a reference image), the preview shows **AI refine** controls: type an instruction ("make it night", "add a hat") and press **Refine**, or use a preset such as **Variation**. The edited image is sent back to the provider and arrives as a new image in the chat, so you can iterate. Refine is hidden for providers that don't accept image input (e.g. DALL-E images, Automatic1111).

**Checklist**

1. **Base URL** is the API base (the in-app field help shows the expected value); a full endpoint URL is also accepted.
2. **Model** is an image-generation model (e.g. `google/gemini-2.5-flash-image-preview`, `black-forest-labs/flux.2-pro`, `x-ai/grok-imagine-image-quality`).
3. **Output** matches the model: "Image only" for image-only models, "Image + text" for dual-output models.
4. **API key** is present and valid; the **Send credential in** selector matches what the provider expects (header, URL query parameter, or request body).
5. The provider is **set active** in **Settings → Providers → Image Generation**.

## 7. Verzeta Studio does not open, or closes straight away

**Symptoms**

- You start the app and no window appears, or it closes after the splash screen. No error dialog is shown.

**Common causes and fixes**

- **Another copy is already running.** Only one Verzeta Studio can use the database at a time. The log says "Another Verzeta Studio engine instance is already running on this database". Close the other window, or stop a headless service that uses the same account (`systemctl --user stop verzeta-studio-headless`). If the other copy crashed, the next start cleans up on its own.
- **The data folder is on a network drive.** Verzeta Studio refuses to open its database on NFS, SMB, SSHFS or similar mounts, because SQLite can corrupt data there. The log says "Database directory is on a network / distributed filesystem". Move the data folder to a local disk. On Linux you can instead start the app with `XDG_DATA_HOME` pointing at a local folder.
- **The database cannot be opened.** The log says "Cannot open database: " followed by the reason. Check that your user owns the data folder and that the disk is not full.

The log is at `~/.local/share/Verzeta/verzeta-studio/logs/verzeta-studio.log` on Linux and `%APPDATA%\Verzeta\verzeta-studio\logs\verzeta-studio.log` on Windows. On Linux the same lines are also printed in the terminal if you start the app from one.

## 8. An agent's shell command is refused

Agents run command-line programs through the shell tool. Refusals appear in the **Tool Log** and are passed back to the agent.

- **"Program 'X' is not in the allow-list".** Agents can only run programs on the allow-list; the message ends with the list of allowed programs. Add the program in **Settings → Execution & Permissions**. See [15-agent-execution-and-permissions.md](15-agent-execution-and-permissions.md).
- **The command is refused even though the program is allowed.** Destructive patterns are always blocked, whatever the allow-list says. The message names the pattern, for example "Recursive rm detected, so the script was blocked." or "sudo detected." Recursive deletes, `sudo` and other privilege escalation, formatting disks, writing to system paths, and shutting down the computer are all refused. Run such commands yourself in a terminal.
- **A command stops after two minutes.** Foreground commands have a 2-minute limit. Long-running programs such as development servers should be started in the background. At most four background processes can run at once; a fifth is refused with "4 background processes are already running (the maximum)."

## 9. Canvas Run problems

- **A banner says "No OS-level sandbox".** On Linux, Run uses bubblewrap. If `bwrap` is not installed, or your kernel blocks it, scripts run directly with your user's permissions. Install bubblewrap from your distribution (for example `sudo apt install bubblewrap`). On Ubuntu 24.04 and later, AppArmor blocks bubblewrap by default. To allow it, run `sudo sysctl -w kernel.apparmor_restrict_unprivileged_userns=0`, and add the same setting to a file in `/etc/sysctl.d/` to keep it after a reboot. Restart Verzeta Studio afterwards. On Windows there is no sandbox, so the banner always appears.
- **"Script blocked by the safety scanner."** The script contains a destructive pattern such as a recursive `rm`, `sudo` or `format`. The console shows which one. Run the script outside the app if you are sure it is safe.
- **"Python interpreter not found on PATH." or "Interpreter 'bash' not found on PATH".** Install Python 3 or bash, or make sure it is on your `PATH`, then restart Verzeta Studio.
- **"Stopped: no output or input for 60 seconds." or "Stopped: exceeded the 5-minute run limit."** These are the Run limits described in [06-canvas-and-tasks.md](06-canvas-and-tasks.md).
- **"The canvas is empty, so there is nothing to run."** Add some code first.
- **There is no Run button.** Run supports Python and Bash only. Other languages show **Open in IDE**.

## 10. The built-in local engine does not reply

This applies to the **Local AI** edition only.

- **"No local model is loaded. Choose a model file in the llama.cpp provider settings first."** Open **Settings → Text Providers**, click **llama.cpp (Local)**, and choose a `.gguf` file under **GGUF Model Path**.
- **"Local inference is unavailable because the verzeta-inference engine is not running."** The local engine runs as a separate program, `verzeta-inference`, next to the main executable. If it fails to start three times in a row, Verzeta Studio stops trying until you restart the app. Check the log for the reason (often a model file that is damaged or too large for your memory).
- **The agent never uses tools.** The local engine does not support tool calling. For agents that need tools, use Ollama, a llama.cpp server, or a cloud provider.
- **Replies stop at about 1,000 tokens.** When **Max Tokens** is **Auto (no cap)**, the local engine caps a reply at 1,024 tokens. Set an explicit value in **Chat Settings → Parameters** for longer replies.
- **Local options say they are not in this build.** The wizard shows "Unavailable in this build" and the Embeddings page shows "Use local model (llama.cpp, not in this build)". You are running the Standard edition. Use a local server such as Ollama, or install the Local AI edition.

## 11. A recommended model download fails

**Download recommended** (in the wizard and on the **RAGP** and **Embeddings** pages) fetches the model from Hugging Face and checks it against a fixed size and SHA-256 checksum before using it. The reason appears under the button.

- **"The download does not match the expected model file (checksum mismatch) and was discarded." or "The download was discarded because of a size mismatch".** The file arrived damaged or incomplete, so it was deleted. Try again. If it keeps failing, a proxy or captive portal may be changing the download.
- **"The file was verified but could not be moved into the models folder".** Check that your user can write to the models folder and that the disk is not full.
- **Nothing happens when you click the button.** Verzeta could not create or write to the models folder. Check its permissions and free space. The routing model needs about 2.5 GB and the embedding model about 150 MB.
- **A network error.** Check that this machine can reach `huggingface.co`. You can also download a `.gguf` file yourself and add it with **Browse…**.

## 12. A self-hosted OpenAI-compatible server does not work

- **Test Connection says "Reachable, but /v1/models was not found."** Add `/v1` to the end of the base URL, for example `http://localhost:1234/v1` for LM Studio.
- **"Reachable, but no models are loaded."** Load a model in the server first.
- **"The server requires an API key." or "The server rejected the API key."** Turn on **Server requires an API key** and check the key.
- **"Cannot reach the server."** Check the URL, that the server is running, and that the port is open.
- **Agents with tools loop or answer in plain text.** The server or the loaded model does not support tool calling. Turn off **Server supports tool calling** for that server.

See [12-self-hosted-servers.md](12-self-hosted-servers.md) for every result and per-stack notes.

## 13. llama.cpp (Remote) returns empty replies

- Check the server is up: `curl http://localhost:8080/v1/models` should list your model.
- If the model is a reasoning model, its text may all be in the reasoning part. Expand **Reasoning** in the reply bubble, or turn off **Thinking** for the chat.
- If you ticked **My llama.cpp build supports tool calling**, the server needs `--jinja` and a model whose chat template supports tools. Otherwise untick it.

## 14. API keys are missing or rejected after moving to a new computer

API keys are stored in the settings file, not in the data folder, and they are scrambled with a value tied to the computer they were entered on. A copied settings file therefore does not bring working keys to another computer. Enter your API keys again in **Settings → Text Providers** on the new computer.

## Voice calls

Voice is an optional add-on with its own setup and failure modes (not
detected, missing speech models, nothing heard or transcribed). See
[16-voice-calls.md](16-voice-calls.md).

## Where to find more detail

If your issue is not covered here:

- Check the log file at `~/.local/share/Verzeta/verzeta-studio/logs/verzeta-studio.log` (Linux) or `%APPDATA%\Verzeta\verzeta-studio\logs\verzeta-studio.log` (Windows). Each line shows the time, the level (warning, error and so on) and the area of the app that wrote it.
- Inspect the activity log for the conversation (see [09-activity-timeline.md](09-activity-timeline.md)); every tool call records its result, including error bodies.
- Open the database with any SQLite browser to inspect `messages`, `tool_calls`, or `activity_log` directly. Path: `~/.local/share/Verzeta/verzeta-studio/verzeta-studio.db`. Close Verzeta Studio first if you plan to change anything.

## Capturing detailed logs for a bug report

By default the log stays quiet. It records warnings, errors, and a few key milestones only, so it does not grow quickly and does not slow the app down. When you are chasing a specific problem, or a maintainer asks for a log, you can turn on full detail for a single run with one environment variable. There is nothing to reinstall and no setting to change inside the app.

**Turn on full detail, then launch Verzeta from the same window:**

- **Linux:** `QT_LOGGING_RULES="verzeta.*=true" ./verzeta-studio`
- **Windows (PowerShell):** `$env:QT_LOGGING_RULES="verzeta.*=true"`, then start Verzeta.
- **Windows (Command Prompt):** `set QT_LOGGING_RULES=verzeta.*=true`, then start Verzeta.

Reproduce the problem, close Verzeta, and send the log file. Closing the window (or unsetting the variable) returns logging to normal.

To focus on one area instead of everything, name it in place of `verzeta.*`:

| Rule | Area |
| --- | --- |
| `verzeta.llm.debug=true` | Model providers and requests |
| `verzeta.infer.debug=true` | The built-in local inference engine |
| `verzeta.rag.debug=true` | Retrieval and embeddings |
| `verzeta.tools.debug=true` | Tool calls (shell, files, web search) |
| `verzeta.remote*=true` | Remote access and device pairing |
| `verzeta.ui.debug=true` | The chat engine and app events |
| `verzeta.db.debug=true` | The database |
| `verzeta.memory.debug=true` | Agent and team memory |
| `verzeta.voice*=true` | Voice calls |
| `verzeta.custom-servers.debug=true` | Custom OpenAI-compatible servers |
| `verzeta.image-providers.debug=true` | Image generation providers |

For the most verbose per-token streaming or workspace-mount traces, also set `VERZETA_STREAM_TRACE=1` and/or `VERZETA_MOUNT_TRACE=1` the same way.

When the log file reaches 5 MB it is renamed to `verzeta-studio.log.1` and a new file starts, so at most two files are kept.

## What's next

- [11-faq.md](11-faq.md): Common questions about intended behaviour.
- [02-providers.md](02-providers.md): Per-provider setup reference.
- [08-remote-android.md](08-remote-android.md): Android client behaviour in detail.
