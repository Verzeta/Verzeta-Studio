# Activity timeline

The activity timeline is Verzeta's audit log. Every agent turn, tool call, file write, canvas edit, member change, and poll event is recorded. You can open the timeline for a project or a conversation.

## Why it exists

In a multi-agent setting, it is easy to lose track of who did what. The activity log answers questions like:

- "Which agent wrote `api.md`?"
- "When did `@Alice` join the project?"
- "Which tools did the team use in the last cascade?"
- "Did anyone vote on that poll?"
- "What was the model and provider that produced this turn?"

Everything is local, append-only, and tagged with timestamps, actor identities (user, agent, client or system), and event details.

## What gets recorded

The timeline records these event types:

| Event type          | When it is recorded                                                           | What's in the detail                                     |
| ------------------- | ----------------------------------------------------------------------------- | -------------------------------------------------------- |
| **agent_turn**      | When an agent's response finishes (any cascade member, any provider)          | provider, model, finish_reason, total_tokens, elapsed_ms |
| **image_generated** | When `generate_image` produces a file                                         | job_id, prompt, local_path                               |
| **tool_invoked**    | When any tool completes (success or error)                                    | tool_name, args summary, status (success / error)        |
| **poll_created**    | When `start_poll` opens a poll                                                | poll_id, question, mode                                  |
| **poll_vote**       | When `cast_vote` is called                                                    | poll_id, option_text                                     |
| **poll_closed**     | When `close_poll` finalises a poll                                            | poll_id, winning_option                                  |
| **member_added**    | When a member is added (user-driven or agent-driven via `add_project_member`) | added_alias, added_agent_id                              |
| **member_removed**  | When a member is removed                                                      | removed_alias, removed_agent_id                          |
| **canvas_edited**   | When an agent's `edit_canvas` completes successfully                          | filename, revision, lines                                |
| **file_written**    | When an agent's `write_file` completes successfully                           | absolute_path, bytes                                     |
| **Workspace events** | When a VS Code workspace is connected, replaced, goes stale, is disconnected, its file tree is refreshed, or its approval tier changes | workspace details |

## Opening the timeline

You can open the timeline for a **project** or a **conversation**. Each shows events filtered to that scope.

### Project scope

1. Find the project folder in the sidebar.
2. Open the folder's menu and choose **Configure…** to open **Folder Settings**.
3. Scroll to **Project Activity → View Activity Log**.

The timeline opens scoped to that project. Every event recorded for this project is shown, newest first.

### Conversation scope

1. Open the conversation.
2. Open **Chat Settings**.
3. Scroll to the bottom **Activity** section → **View Activity Log**.

The timeline opens scoped to the conversation. Every event recorded for this conversation is shown, newest first.

## Filtering

In the timeline overlay you can filter by:

- **Actor**: all actors, users, agents, system, or clients.
- **Event**: all events, agent turns, tools invoked, polls (created, votes, closed), members added or removed, files written, canvas edits, images generated, permission denials, and deliverables.

Filtering happens in the overlay without re-querying the database, so changing a filter is instant. The row count in the header updates to match.

## Reading a row

Each row in the timeline shows:

- **Actor kind pill**: `user`, `agent`, `client` or `system`, each in its own colour.
- **Actor alias**: the @mention name of the actor, in monospace.
- **Timestamp**: when the event happened, in your local time zone.
- **Event type label**: human-readable name of the event.
- **Tool name pill**: when relevant (e.g. `write_file`, `start_poll`).
- **Event summary**: one human-readable sentence describing what happened.
- **Event detail** (expandable): full key/value detail of the event in monospace.

Click any row to expand it and see the full event detail, pretty-printed.

## Live refresh

The list updates when a new event is recorded, and also checks for new events every 5 seconds while it is open. Use **Refresh** to reload the list at any time.

## Persistence

The activity log is **append-only**. Rows are never deleted automatically.

- Stored in the `activity_log` table of the local SQLite database.
- Indexed by project, conversation and turn for fast scoped queries.
- Each row is usually under 1 KB.

If you want to trim the activity history, close Verzeta Studio, back up the database, and then delete rows from the `activity_log` table with any SQLite tool.

## Access on Android

The Android client can view the project-scoped timeline. Open a project folder on Android, then **View Activity Log**. Same content as the desktop.

## What's next

- [04-multi-agent-teams.md](04-multi-agent-teams.md): Cascading agents that generate the events you see in the timeline.
- [05-projects.md](05-projects.md): Where to open the project-scoped timeline from.
- [08-remote-android.md](08-remote-android.md): Activity timeline access on Android.
