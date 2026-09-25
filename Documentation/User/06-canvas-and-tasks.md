# Canvas and tasks

This page covers the **canvas**, a side editor for documents and code, and **tasks**, which keep long multi-step work on track.

## Canvas

The canvas is a side editor that opens alongside the chat. Agents use it to draft and edit code, configuration, specifications, or any structured text. You can edit alongside the agent; changes are tracked with revisions.

### How agents open a canvas

An agent calls the **`open_canvas`** tool:

```
open_canvas(filename="api.md", language="markdown", content="# API\n...")
```

The canvas appears beside the chat, opened to that file with syntax highlighting for the declared language. Press Ctrl+Shift+C to show or hide it.

> **Note**: Only one canvas is "active" per conversation at a time. Opening a new canvas archives the previous one. Archived canvases are visible in the canvas history dropdown.

### Editing in the canvas

You can type in the canvas yourself; changes are saved as revisions. The agent's `edit_canvas` tool also updates the file. Both updates increment the revision number.

The canvas is auto-saved as you type. There is no manual "save": every edit is committed to the DB and to a mirror file on disk in the conversation's artifact directory (or the project's artifact directory for project-scoped canvases).

### AI actions

The bottom toolbar of the canvas has **AI Actions**: language-aware shortcuts that send a structured request to the agent. Examples:

- For code (Python, JavaScript, etc.): **Add Comments**, **Fix Bugs**, **Refactor**, **Port to Language…**.
- For prose (Markdown, plain text): **Fix Grammar**, **Translate…**, **Paraphrase**, **Change Tone…**.
- For data (JSON, YAML): **Convert…**, **Generate Schema**, **Validate Against Schema**.

Each action sends the canvas content and a structured instruction to the conversation's active agent. The agent replies with an edited version, which lands as a new canvas revision. **Port to Language…** and **Convert…** open the result as a new canvas file instead of a new revision.

### Running code in the canvas

For **Python** and **Bash** canvases, a **Run** button is available.

#### Per-platform sandbox behaviour

| Platform    | Sandbox                                                                                                                                                      |
| ----------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| **Linux**   | `bubblewrap` (`bwrap`): no `/home` access, no `sudo`, read-only system directories. The script does keep network access (see Sandbox limits below). If bubblewrap is missing or the kernel blocks it, scripts run directly with your user's permissions and a warning banner appears. |
| **Windows** | Direct subprocess. There is no native sandbox; safety relies on a scanner that refuses scripts containing known dangerous patterns. A warning banner appears above the canvas. |

> **Warning**: Without bubblewrap there is **no kernel-level sandbox**. The dangerous-pattern scanner is a pre-flight check, not isolation. Treat those scripts as if they ran with your user's full privileges. Run untrusted code only on Linux with bubblewrap working (no warning banner).

#### Sandbox limits

- A run stops after 60 seconds with no output and no input from you. Anything the script prints, and anything you type, resets that window, so a script waiting at a prompt will not be cut off while you are reading or typing.
- A run also stops after 5 minutes in total, no matter how busy it is. The Stop button ends a run at any time.
- Scripts can reach the network. The sandbox shares your machine's network connection, so a script can download files. It cannot keep anything it installs, because the system directories are read-only and its temporary space is discarded when the run ends.
- No `/home` access on Linux; the sandbox sees only the scratch directory (only when bubblewrap is active).
- Output appears in the canvas console panel below the editor.

For errors such as "Script blocked by the safety scanner." or a missing interpreter, see [Canvas Run problems](10-troubleshooting.md#9-canvas-run-problems).

#### Scripts that ask for input

Scripts that read from the keyboard work. When a Python script calls `input()`, or a Bash script uses `read`, the prompt appears in the console and an input box opens at the bottom of the console panel.

- Type your answer and press Enter to send it to the script.
- Pressing Enter on an empty box sends a blank line, the same as pressing Enter at a terminal prompt.
- The **End input** button (tooltip **End input (EOF)**) tells the script there is no more input, for scripts that keep reading until the input ends.
- Lines you type are shown in the console so you can read the run back as a conversation.

This works from a paired client too. If you start a run from the Verzeta app on your phone, the prompt appears in that console and you can answer it there. The script still runs on this machine, with the same sandbox rules.

One thing to know: a Bash script that writes its prompt with `read -p` will not show that prompt, because that text goes to the error stream, which the console only shows when a run fails. Print the prompt first (for example `echo -n "Name: "`) and then call `read`.

#### Other languages

For canvases in other languages (C++, Go, TypeScript, Rust, etc.), the Run button is replaced with **Open in IDE**. Clicking it opens the canvas file in your system's default editor for that language.

### Canvas history

Click the canvas filename in the top bar to open the **history dropdown**. You can see every canvas that has ever been opened in this conversation, with revision counts and timestamps. Click any history entry to switch back to it; the agent's next message sees that canvas as active.

## Tasks

A task is a shared goal the whole conversation works toward. It keeps
long, multi-part work anchored: every turn, the agents are reminded
what the open task is and what has been produced so far, so the goal
survives long conversations, context refreshes, and app restarts.

### How a task starts and ends

An agent calls **`start_task`**, or you use the **Start a Task** button
in the message bar (in the **More** menu when the window is narrow),
with a goal and an optional list of steps. A plan card appears in the
chat and the task stays open while the team works.

The task belongs to the **whole conversation**. There are no per-step
owners and no gatekeeper. When the work is done, **any participant**
closes it by calling **`complete_task`**; a visible receipt shows who
closed it and why ("✅ Task completed by @…"). Agents can also check
progress with `get_task_status`, and you (or an agent) can abandon a
task with `stop_task`.

By default a task stays open until someone completes it. If you turn on
**Auto-complete tasks when the model goes quiet** in Chat Settings, a
task is also marked done when the model stops right after a tool call.

### Tasks survive restarts and switches

An open task is persistent:

- **Switching conversations** does not touch it. When you return, the
  task is re-anchored automatically.
- **Closing and reopening the app** does not touch it either. The task
  is stored with the conversation and picks up where it was.

A task only ends when someone completes it, stops it, or you delete
the conversation.

### Artifacts

Everything produced while a task is open is recorded against it and
shown in the **Artifacts** panel and the Plans overlay:

- Files written by agents (`write_file`, canvas saves).
- **Generated images**: each image an agent creates is saved into the
  project's `images/` folder and listed as an artifact alongside the
  files, with the chat message noting where it was saved.

Each row in the Artifacts panel has three actions:

- **Open in Canvas** (for text files such as Markdown, code,
  configuration files and logs): loads the file into the conversation's
  Canvas for reading or editing. Re-opening a file that already has a
  canvas updates that canvas with the file's current content as a new
  revision.
- **Open with default application**: opens the file with whatever your
  system uses for that type: your text editor for documents, your
  image viewer for generated images.
- **Copy full path**: copies the file's complete location on disk, so
  you can paste it into a terminal or another application.

Artifacts remain browsable after the task completes.

### The Plans overlay

Click **Plans** in the chat header to open the Plans overlay. It lists
every task in the conversation with its status, its steps if it has
any, and the files it produced. For a step you can choose **Mark Done**,
**Skip**, or **Retry** (with an optional note for the agent). **Stop
plan** stops the whole task. To start a new task, use **Start a Task**
in the message bar.

### When to use tasks vs a group cascade

| Situation                                              | Use                     |
| ------------------------------------------------------ | ----------------------- |
| Open-ended discussion with 2 to 4 agents               | Group cascade (no task) |
| Long, structured work that must survive many turns     | Task                    |
| Quick exchange where the model can finish in one reply | Send the message        |

> **Pro-tip**: You can combine them. A coordinator agent in a group
> chat can `start_task` mid-conversation; the team keeps collaborating
> normally and anyone closes the task when the goal is met.

## What's next

- [04-multi-agent-teams.md](04-multi-agent-teams.md): Coordinator-driven tasks in a team setting.
- [07-skills.md](07-skills.md): Equipping the agent with installable instruction packs.
- [10-troubleshooting.md](10-troubleshooting.md): When agents loop on tool calls or seem stuck.
