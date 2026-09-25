# Quick start

This guide takes you from a fresh install to your first AI conversation.

## Before you begin

You need **one** of the following:

- A working [Ollama](https://ollama.com) install on your machine (easiest, fully local), **or**
- An API key for any one of: OpenAI, Anthropic, Google Gemini, OpenRouter, DeepSeek, **or**
- The **Local AI** edition of Verzeta Studio, which includes the on-device inference engine (no server and no account: download a model in the wizard and run everything locally).

You do not need all of them. You can add more providers later.

### Which download did I get?

Each release ships two editions per platform:

| Artifact | What it includes |
| --- | --- |
| `Verzeta-Studio-<version>-x86_64.AppImage` / `…-windows-x64.zip` | The Standard edition. Connects to cloud providers and remote or self-hosted servers. |
| `Verzeta-Studio-<version>-localai-x86_64.AppImage` / `…-windows-x64-localai.zip` | The Local AI edition: everything above **plus** the bundled local inference engine (GPU-accelerated via Vulkan when your machine has a GPU, automatic CPU fallback when it doesn't). Models are **not** bundled. You pick or download one inside the app. |

## Step 1: Launch the app

Open **Verzeta Studio**. On first launch you will see the **Get Started** wizard.

> **Note**: If you skip the wizard, you can return to it from **Settings → Onboarding → Re-run Welcome Wizard** at any time.

### Resizing the window

The window adapts as you make it narrower, so the app is usable on a small
screen or a phone-sized window as well as a full desktop one.

- **Wide**: the navigation bar, the conversation list and the settings panel
  all sit side by side.
- **Narrower**: the navigation bar and conversation list move into a drawer
  you open with the menu button in the top left, and the canvas gets its own
  tab instead of sharing the screen with the chat.
- **Phone width**: cards stack their controls, tab labels shorten, and
  secondary tags on provider and agent cards are hidden so names and
  descriptions keep the room they need. Everything hidden this way is still
  available once you widen the window again.

## Step 2: Configure one provider

In the wizard's **Providers** step, you will see these sections:

- **Cloud Providers**: OpenAI, Anthropic, Google Gemini, OpenRouter, DeepSeek
- **Remote Servers**: Ollama, llama.cpp (Remote)
- **Local Models**: the bundled local inference engine (Local AI edition)
- **Custom OpenAI-Compatible Servers**: any other server that speaks the OpenAI API (see [12-self-hosted-servers.md](12-self-hosted-servers.md))

Pick one to start and click its card.

- For **Ollama**: leave the base URL at `http://localhost:11434` (the default). Click **Test Connection**. You should see a green check.
- For a **cloud provider**: paste your API key. Click **Save**.
- For **llama.cpp (Remote)**: enter the URL of your running llama.cpp server (default `http://localhost:8080/v1`).

> **Pro-tip**: You can configure more providers later from **Settings → Text Providers**. The wizard only needs one to let you continue.

## Step 3: Local Intelligence (optional, skippable)

The wizard's **Local Intelligence** step sets up two optional on-device models. Both are only offered in the **Local AI** edition; in the Standard edition the step points you to your remote providers.

- **Routing model**: decides which agent replies next in group chats. Choose **Use the on-device model** and either pick a `.gguf` file you already have (**Browse…**) or click **Download recommended** (~2.3 GB, verified against a pinned checksum). Runs on your GPU via Vulkan when available, otherwise on CPU.
- **Embedding model**: powers RAG retrieval from your messages and documents. Same idea: **Browse…** a `.gguf` or **Download recommended** (~139 MB).

Nothing downloads unless you click the button, and no models are ever bundled with the app. Downloads come from Hugging Face.

> **Note**: You can change all of this later in **Settings → Providers → RAGP (Routing & Gating)** and **Settings → Providers → Embeddings (RAG)**. Both pages have the same download button.

If you are only using solo chat with a remote provider, skip this step entirely.

## Step 4: Send your first message

Click **Done** to finish the wizard.

On the home screen, click **New Chat**.

In the input field at the bottom, type a message such as:

```
Tell me one interesting fact about the Voyager 1 spacecraft.
```

Press **Enter** to send. (**Shift+Enter** for a newline.)

The reply appears as it is generated. While it streams, the **Send** button turns into **Stop**, which cancels the reply. Afterwards, **Retry last response** in the message bar sends your last message again for a new reply.

## Step 5: Try a group chat

On the home screen, click **New Group Chat**.

In the **New Group Chat** dialog:

- Enter a **Title**.
- Under **Members**, click **Add Member**, pick an agent template (e.g. Researcher, Code Reviewer, Designer) and give it an alias. Add at least two members.
- For each member, you may optionally press the configure button on its row (**Configure model / tools for this member**) to set a different **Provider**, **Model** or tool whitelist.
- Tick **Mark as coordinator** on one member (suggested: the first one).
- Click **Create**.

Send a message addressed to the coordinator, such as:

```
@Alice, research two Python CLI libraries and tell me which to use.
Cite real sources.
```

Watch the conversation cascade. Agents @mention each other, hand off work, and the coordinator drives the team.

> **Note**: Each round allows **10 agent turns** and **3 turns per member**. While the team makes progress it starts new rounds on its own, up to 6 by default. When it stops, the chat says why. Reply with anything to start it again.

## What's next

- Open **Chat Settings** in a conversation's header to change the model, system prompt, or tools for that one chat. See [03-conversations.md](03-conversations.md).
- Open the **Activity Log** for your conversation to see the audit trail. See [09-activity-timeline.md](09-activity-timeline.md).
- Pair an Android device. See [08-remote-android.md](08-remote-android.md).
- Install a skill so agents can use specialised knowledge. See [07-skills.md](07-skills.md).
