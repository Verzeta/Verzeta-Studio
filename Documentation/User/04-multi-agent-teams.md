# Multi-agent teams

A group chat is one conversation in which several named agents work together, each using its own model, role, and tool whitelist. Agents @mention each other, replies cascade automatically, polls let the team make decisions, and the activity timeline records what happened.

## Vocabulary

| Term               | Meaning                                                                                                                                                                            |
| ------------------ | ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Agent template** | A reusable agent definition (name, description, system prompt, optional model override). Managed on the **Agents** page in the navigation bar.                                    |
| **Member**         | An instance of an agent template inside a specific conversation or project. A member has an alias (`@Maya`, `@Clark`) and per-member overrides for provider, model, and tools. |
| **Alias**          | The `@mention` handle of a member, unique within its conversation or project. Set when you add the member.                                                                        |
| **Coordinator**    | A member flagged as the team's driver. Has permission to add other members via tool calls; typically receives the user's initial nudge.                                            |
| **Cascade**        | The chain of agent replies triggered by one user message. Each round allows **10 turns** in total and **3 turns** per member. A cascade can run several rounds (see [Turn limits, rounds, and pauses](#turn-limits-rounds-and-pauses)). |
| **RAGP**           | Routing and Gating: the classifier that decides which member should reply next. It can run on the built-in engine or on a remote Ollama model (**Settings → Providers → RAGP (Routing & Gating)**). |

## Creating agent templates

Open **Agents** in the navigation bar. It lists the built-in templates and **Your Agents**. Click a template to start a chat with it, or click **New Agent** to create one. A template needs a name and a system prompt; you can also set a description, an icon, a model override, a default reasoning pattern and a default heartbeat suggestion. Every member you add to a chat or project starts from a template.

## Creating a group chat

1. On the home screen, click **New Group Chat**.
2. In the dialog:
   - Enter a **Title**.
   - Click **Add Member**. Pick an agent template (e.g. Researcher).
   - Give the member an alias (e.g. `Maya`). The alias is how you @mention it.
   - Optionally press the configure button on the member's row to set per-member overrides: a different **Provider**, a different **Model**, or a restricted **Tool Whitelist**. Leave them empty to use the conversation's defaults.
   - Repeat for as many members as you want. A group chat needs at least 2 members; typical teams have 2 to 6.
   - Tick **Mark as coordinator** on one member. The coordinator drives the conversation.
3. Click **Create**.

The chat opens. Send the first message addressed to the coordinator:

```
@Alice I need a short report on two Python CLI libraries (typer vs click).
Two members of the team should research; one should write a one-paragraph
recommendation; vote on the choice if you disagree.
```

## Managing members of an existing group chat

Open the conversation and open **Chat Settings** (the gear button in the chat header). The **Group Members** section shows every member with its alias, role, and model summary, and is the one place to manage *this chat's* roster:

- **Add Member**: opens the add dialog. If the chat lives inside a project or organization folder, the dialog first offers **existing project members who are not in this chat yet**, and one click adds them under the same alias they already have in the project, keeping their configured model and tools. Below that you can still pick any **agent template** to create a new member.
- **Remove from this chat** (🗑 on a member's row): removes the member **from this chat only**, after a confirmation. Their earlier messages stay in the conversation, and they remain in the project's roster. The last remaining member cannot be removed, because a group chat needs someone to answer.
- **Mark as coordinator** (⭐ on a member's row): moves the chat's coordinator role to that member. There is always exactly one coordinator per chat.
- **Configure** (on a member's row): the per-member provider, model and tool overrides described below.

When the chat belongs to a project or organization, a **Project Roster…** button appears under Add Member. It opens the folder's settings, the same dialog you reach with **Configure…** in the folder's menu in the sidebar, where you add or remove members of the **project itself**, change the *project* coordinator, and edit roster-level overrides. The panel changes this chat's members. **Project Roster…** changes the project's members.

> **Note**: An agent coordinator can also grow the team by tool call (`add_project_member`). When it does so from a group chat, the new member joins both the project roster **and** that chat, ready to be @mentioned immediately.

## How a cascade works

When you send a message, Verzeta:

1. Parses the @mentions in your text to find the primary responder.
2. Sets the responder's identity (alias, agent template and per-member overrides).
3. Dispatches the LLM request using the responder's assigned provider and model.
4. As the assistant streams a reply that mentions another member, the **RAGP** classifier inspects the reply and queues the next responder.
5. When the current reply finishes, the queue drains: the next member's turn starts (its own provider, model, and tools).
6. Continues until the queue is empty, or the round's limits are reached (10 turns in total, 3 per member). If the round made progress, a new round starts; see below.

> **Note**: The cascade is _deterministic_ in structure (each step is logged and inspectable) but the _content_ of each reply comes from the model. Agents can disagree, change their minds, or skip a tool you expected. That depends on the models, not on how the cascade works.

## Per-member overrides

Members of one team can use different providers and models.

> **Example**: A 4-member team:
>
> - `@Alice` on Ollama with `qwen3:14b` (runs locally)
> - `@Maya` on Claude
> - `@Robin` on GPT-4o
> - `@Clark` on Gemini
>
> All four members coordinate in one chat. Each turn dispatches to its assigned provider and model.

To set per-member overrides:

- **Project members**: open the folder's **Folder Settings → Team Members**, then set **Provider**, **Model**, and tool overrides for the member.
- **Group chat members**: open **Chat Settings → Group Members** and press the configure button on the member's row.
- For a 1-on-1 direct chat with a project member, the override comes from the project member's row (not the conversation), so a change to the project member updates the 1-on-1 too.

> **Pro-tip**: When you copy the same agent template into a project as multiple members (e.g. three Writer aliases), each member can have a different provider override. You get three differently-modelled writers from one template.

## Tool whitelist per member

Each member can have a list of allowed tools (**Tool Whitelist** in the member's configure dialog). When the list is not empty, the member sees **only** those tools, even if other tools are registered globally.

Use this to restrict members:

- `@Robin` allowed only `read_file`, `list_files`, `get_current_time`: cannot edit files or run shell commands.
- `@Alice` allowed everything: full team coordinator.

> **Note**: The whitelist intersects with the conversation's normal tool gating. An empty list means no restriction. Otherwise the member can use only the listed tools.

## @mentions

| Mention                              | Behaviour                                                                             |
| ------------------------------------ | ------------------------------------------------------------------------------------- |
| `@Alice`                             | Direct to a single named member.                                                      |
| `@user`, `@owner`, `@you`, `@leader` | Sends a notification to the human user; does not cascade.                             |
| `@all`, `@everyone`, `@team`         | Broadcast to all other members. Each member who has not already spoken gets one turn. |

The classifier handles **intent** as well. If a message contains an @mention but is only a reference ("I'll let `@Alice` know"), the cascade does **not** fire. If it is a real handoff ("`@Alice`, please draft the intro"), it does.

## Turn limits, rounds, and pauses

To stop runaway loops, each round of autonomous team activity has
limits: **10 agent turns total per round** and **3 turns per
individual member**, plus a limit of 25 tool calls within a single
turn by default (**Tool steps per turn** in Chat Settings).

What happens when the limits are reached depends on whether the team
is getting work done:

- **The round made progress**: a member used a tool, wrote a file,
  made a real correction, or contributed something new to the
  discussion, and someone is still addressed. The team automatically
  continues into a new round. You'll see a small entry in the chat,
  e.g. `▶ Turn limits reached, continuing automatically (round 2/6),
  next: @Robin`. The second number in this notice is always 6, whatever
  the conversation's round setting. A plain back-and-forth discussion
  with no tools keeps going on its own. Only a round that adds nothing
  new is treated as "no progress".
- **No new progress** (the agents only repeated themselves), or the
  conversation's round budget is used up. The team pauses and tells
  you why. With no progress:
  `⏸ Agent turn limits reached with @Robin still queued. This round ran
  no tools, so the team is pausing to avoid a loop. Reply with anything
  (for example "continue") to start a new round.` When the round budget
  is used up: `⏸ 6 automatic rounds completed; @Robin is still queued.
  Reply to continue.` This notice also always says 6.

When a multi-turn round finishes cleanly, with every handoff answered and
nobody left holding work, the chat says so:

> `✔ Round settled with no further handoffs after 6 agent turn(s). The
> team is waiting for you.`

Either way, the chat always says why the team stopped. Any message from
you, even a single word, starts a new set of rounds.

### When a member says it will do something but doesn't

Sometimes a member announces an action, such as "I'll create pricing.md
right away", and then stops without using a tool. The routing model
checks whether the member really left its own work undone. Repeating an
earlier announcement does not count. If the work is undone, Verzeta asks
that member to follow through:

> `↻ @Alice described a file/tool action but didn't run it, so they
> are being asked to do it now (or hand it off).`

The member's original message is **kept**, nothing is deleted, and
the member either runs the tool, hands the work to a teammate with an
@mention, or explains why it can't. Handing work off ("@Clark, please
create pricing.md") is never treated as a stall. That is normal
delegation, and the teammate gets the next turn. This also works when
the handoff is indirect: "Thanks @Clark, great summary! Please go ahead
and run the tests now" passes the turn to Clark. Each member is asked
at most a few times per exchange, so this cannot loop. Your own
messages always take priority: if you type while a follow-through is
pending, the team stops and listens to you first.

### When a member speaks in someone else's voice

If a member's reply arrives written as another member, opening as if
a teammate had said it, Verzeta Studio rejects that reply instead of
filing it under the wrong name: the bubble is removed and the member is
automatically asked to answer again as themselves. The wrong text may
appear briefly before it is removed. After three failed attempts the
reply is kept and marked so the conversation can continue. A reply kept
this way carries the note "Echo-failed: this agent could not produce an
original reply after multiple retries."

### Team autonomy (rounds before pausing)

How many of those automatic rounds the team runs before pausing for you
is set **per conversation** in **Chat Settings → Agent → "Team autonomy:
rounds before pausing"**:

- **Default 6**: the team works through several rounds, then checks in
  with you.
- **Higher** (up to 50): more independence before it pauses; good for a
  "go research and draft the whole thing" team chat.
- **Unlimited** (set the box to *Unlimited*): the team keeps going until
  it stops making progress on its own (a round that adds nothing new
  ends it), so you only step in when it runs out of new work. A limit
  of 50 rounds still stops an endless loop.

Lower it when you want to steer the team every couple of exchanges;
raise it (or set Unlimited) when you want them to run with a task.

## Sub-agents: letting an agent delegate

Agents in any conversation can hand a self-contained side-task to a
private **sub-agent**: a background worker that reads or researches and
then reports back. By default it can only use read-only tools. You will
see it in two places:

1. The spawning agent's tool call (`spawn_subagent`) appears like any
   other tool use, acknowledging the worker has started.
2. When the worker finishes, its report is posted into the
   conversation as a system entry,
   `🤖 Sub-agent report for @Alice (run 1a2b3c4d): …`, visible to
   you and the whole team, **and the agent that delegated the work
   automatically responds to it**, picking up where it left off. If
   the team is mid-discussion when the report arrives, the reply
   waits for the current exchange to finish (your own messages always
   take priority).

Guard-rails are built in: sub-agents can't spawn further sub-agents,
they only get the tools their parent grants (writing files requires an
explicit grant), at most two run at once (more queue up), and every
run has hard time and step limits. Their full working transcript is
stored with the run rather than cluttering your chat.

## Polls: coordinated decisions

When the team needs to choose between options, agents can open a poll instead of looping in discussion.

### Poll lifecycle

1. An agent (typically the coordinator) calls **`start_poll`** with a question and a list of options:
   ```
   start_poll(question="Pick Typer or Click?", options=["Typer", "Click"], mode="single")
   ```
2. A poll-card message appears in the chat. The poll has an open state and a deadline.
3. Other agents call **`cast_vote`** with their preferred option.
4. The coordinator (or any agent) calls **`close_poll`** to finalise. The winning option is recorded.
5. The team continues with the decision made.

> **Note**: Polls support **single-choice** and **multi-choice** modes.

### Voting from the human side

You can vote in a poll from the chat UI. Click an option on the poll card; your vote is recorded as yours in the activity log. Useful when an agent asks you to break a tie.

## Activity timeline

Every cascade event is recorded:

- Each agent turn (with provider, model, finish reason, token count and elapsed time)
- Each tool call (with an arguments summary and status)
- Each member added or removed
- Each poll created, vote cast, or close
- Each file written by a tool
- Each canvas edit by a tool

See [09-activity-timeline.md](09-activity-timeline.md) for how to open and filter the timeline.

## What's next

- [05-projects.md](05-projects.md): Organising team conversations into projects with shared documents.
- [07-skills.md](07-skills.md): Equipping the team with installable instruction packs.
- [09-activity-timeline.md](09-activity-timeline.md): Inspecting what the team did.
