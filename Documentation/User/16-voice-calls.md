# Voice calls

Talk to your agents out loud instead of typing. In a one-to-one chat you
speak and the agent answers in a synthetic voice; in a group chat each
member gets its own voice and speaks in turn.

Voice is an **optional add-on**, and it is **not released yet**. Without
it, chats show no voice controls; only the **Voice Calls** card in
Settings appears, showing that the add-on is not installed. The rest of
this page describes how voice calls work once the add-on is available.

## What you need

The **Verzeta-Voice** add-on, a separate download.

It is a separate program because it is large (speech models and their
runtimes) and licensed differently (GPL-3.0-or-later). It runs on your
machine and talks to Verzeta Studio locally. Nothing about your voice or
your conversations leaves your computer.

You also need a **microphone**, and **speakers or headphones**.

## Installing

1. Build or download `verzeta-voice` (see that project's README).
2. Install it, or copy it to one of these folders. Verzeta Studio scans
   the usual places automatically: a system install (`/usr/bin`,
   `/usr/local/bin`, `Program Files`), `/opt/verzeta-voice`, `~/.local/bin`,
   `~/Applications`, your `Downloads` folder, and the per-user folder
   shown in **Settings → Voice Calls**. A Linux **AppImage** is detected
   by name, so `Verzeta-Voice-1.0.0-x86_64.AppImage` works as downloaded
   with no renaming. Any folder on your `PATH` also works.
3. Press **Detect**. The line turns green and shows where it was found.
   You do not need to restart Verzeta Studio.

If it is installed somewhere unusual, press **Locate…** and pick the
program in the file dialog. That button only appears while nothing has
been detected, and **Clear custom path** returns to the normal search.

### Speech models

The add-on ships without models, so you choose the quality and size you
want. Download them in **Manage Voice Calls → Downloads** (see below), or
follow the download commands in the add-on's README and put them in the add-on's
models folder (`~/.local/share/verzeta-voice/models` on Linux). You need
two things:

- one **speech-to-text** model (a whisper `ggml-*.bin` file), and
- at least one **voice** for speech output (a piper `.onnx` file with its
  matching `.onnx.json`).

Add more voices to give each agent a different one.

## Turning it on

The **Voice Calls** section in Settings shows whether the add-on is
installed and running, and **Manage Voice Calls** opens everything
else: a switch to start and stop the service, Detect and Locate for
finding the program, the start-with-application option, the default
voice, downloads, and the push-to-talk key.

If the add-on is a different version than Verzeta Studio expects, the
status line says so and names both versions. Update the add-on.

## Making a call

A **call button** appears in the chat header once the service is
running. Click it and the call window opens over the chat, showing a
tile for you and one for each agent.

- **Hold to talk.** Press and hold the talk button while you speak, then
  release. Holding it also stops whatever an agent is saying, so this is
  how you interrupt: press, speak, release. The agent stops talking and
  answers what you just said instead of finishing first.
- **Stop speaking** cuts an agent off without you having to say anything,
  for when you can already tell the answer is going the wrong way. Only
  the audio stops; the reply stays in the chat where you can read it.
- **Who you are talking to.** In a group chat, the dropdown beside the
  talk button chooses one agent or everyone. Verzeta adds the mention for
  you, so you never have to say "at" out loud. The choice sticks until
  you change it.
- **Minimize** collapses the call to a small bar so you can read and use
  the chat while the call carries on. The bar keeps the talk button, stop
  speaking, and hang up. Click restore to bring the full view back.
- **End call** closes the call. The conversation stays; everything said
  is in the chat as ordinary messages.

Only one call can run at a time.

There is no mute button. The microphone is open only while you hold the
talk button, so letting go already means nothing is being heard.

### Push to talk, and why

The microphone is open only while you hold the talk button. That keeps
the agents from hearing themselves through your speakers and answering
their own voices. Always-open listening needs echo cancellation and is
planned for a later release.

### How group calls take turns

In a group call each agent finishes **speaking** before the next one
starts thinking. This is deliberate: it keeps what you hear in the same
order as the conversation, so when you interrupt, the reply you get is to
what you said. It does mean a small pause between speakers while
the next agent composes its answer.

A one-to-one call does not wait like this. There is no next speaker to
order against, so you can talk or type at any point, including while the
agent is still reading its answer out.

## Choosing voices

Voice choices live with the rest of an agent's settings:

- **The default voice** is in **Settings → Voice Calls → Manage Voice
  Calls**. Any agent without a voice of its own uses it.
- **In a group chat**, open **Chat Settings → Group Members**, press the
  configure button on a member, and pick its **Voice** next to its model
  override. The choice applies to that member in that chat.
- **In a one-to-one chat**, **Chat Settings** has a **Voice** section
  right under **Model**.

The voice controls appear only while the voice service is running, and
a change during a call is heard on that agent's next spoken reply.

### Downloading voices and speech models

**Manage Voice Calls** has a Downloads panel with ready-to-use voices
(male and female, US and UK English) and better speech-recognition
models. Each row shows the size and the licence; every download is
checked against a known fingerprint before it is installed, and the
voice service restarts by itself so the new voice appears right away.

A larger speech-recognition model understands you noticeably better
than the starter one, and the best installed model is used
automatically on the next call.

## The push-to-talk key

The on-screen talk button always works. If you prefer a key, set one in
**Manage Voice Calls → Push to talk**: click the field and press the
key combination. Hold it during a call to talk, exactly like holding
the button. The key is active only while a call is running and never
fires while you are typing in a text field.

## What gets spoken

Only what an agent says, and only the parts worth hearing.

Code blocks are read as "Code omitted." A table is read as a one-line
summary of its columns and row count, for example "A table of Feature
and Price with 3 rows is on screen." Read out in full they would be a
stream of punctuation. Both stay fully visible in the chat. Tool calls,
thinking, and system notices are never spoken, and a turn that only ran a
tool passes silently.

## Troubleshooting

**"Verzeta-Voice is not detected" after installing.** Check that the program is in one of
the folders named in Settings, and that it is executable. On Linux:
`chmod +x verzeta-voice`. Press **Detect** again.

**"Verzeta-Voice speaks protocol vX but this app needs vY".** The add-on
and Verzeta Studio must agree on the protocol version. Update the add-on to the release that
matches your Verzeta Studio version.

**"Speech models are missing" when you start a call.** The add-on found
no whisper model or no voice. Download them in **Manage Voice Calls →
Downloads**, or see "Speech models" above; the message names the exact
folder it looked in.

**"Voice service start failed repeatedly".** Verzeta Studio stops
retrying after three failed starts. Fix the cause shown in the log (see
"Collecting a log" below), then press **Detect** to try again.

**"Start the voice service before calling."** Turn the service on in
**Settings → Voice Calls → Manage Voice Calls**, or turn on **Start the
voice service with the application**.

**"End the current call first."** Only one call can run at a time. End
the call in the other chat first.

**Nothing is heard.** Check your system output device and volume. In the
call window, an agent's tile lights up while it is speaking; if tiles
light up but you hear nothing, the problem is the output device.

**Nothing is transcribed.** Check the microphone in your system settings.
Hold the talk button for at least half a second. Presses shorter than
about a third of a second are ignored on purpose, so an accidental tap
sends nothing.

**Sound comes from one speaker only.** Update the add-on. A single-channel
stream plays on the left side only on some systems.

**Every agent sounds the same.** Only one voice is installed. See
"Choosing voices" above.

**The agents interrupt themselves.** Use headphones. Speaker audio
reaching the microphone is what the push-to-talk design avoids, but a
very loud room can still confuse recognition.

**Collecting a log for a bug report.** Start Verzeta Studio with detailed
logging (see [10-troubleshooting.md](10-troubleshooting.md)) and add the
voice area:

```
QT_LOGGING_RULES="verzeta.voice*=true" ./verzeta-studio
```

The add-on writes its own diagnostics to its error output, which is
captured in the same log.

## What's next

- [10-troubleshooting.md](10-troubleshooting.md): general troubleshooting.
- [04-multi-agent-teams.md](04-multi-agent-teams.md): building the team you
  will be talking to.
