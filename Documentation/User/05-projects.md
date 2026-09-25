# Projects

A project is a folder that groups related conversations under a shared goal, a shared team roster, and shared documents. Conversations inside a project inherit the project's member list and can read its documents on every turn.

## When to use a project

| Use case                                                    | Use a project?                                   |
| ----------------------------------------------------------- | ------------------------------------------------ |
| One-off question to a single agent                          | No: use **New Chat**.                           |
| One-off multi-agent discussion                              | No: use **New Group Chat** (no project folder). |
| Multi-week collaboration with a stable team and shared docs | Yes: create a project.                          |
| Recurring drafting / coding / research with the same team   | Yes: create a project.                          |

## Folder types

A folder can be one of three types:

| Type                    | Purpose                                                                                                        |
| ----------------------- | -------------------------------------------------------------------------------------------------------------- |
| **Regular folder**      | Only organises conversations. No team roster, no shared documents.                                             |
| **Project folder**      | A team workspace. Has a goal, a member roster, and project documents. Conversations inside inherit the roster. |
| **Organization folder** | A higher-level folder that gives shared context to the projects inside it. It has the same settings as a project. |

## Creating a project room from the Home page

The quickest way to start a project is from the **Home** page.

1. Click **Project Rooms**. The overlay lists your existing rooms and the templates.
2. Under **Start a project room**, choose **+ Blank project**, or **⚡ Quick start from template** to open the **Template Library** (filter by Sales, Engineering, Marketing, Exec or Discovery). You can also click a template in the list below.
3. Enter a **Project name** (required), a goal, and optional context, and review the **Teammates** the template suggests.
4. Click **✓ Create room**. Verzeta creates the project folder with that team.
5. When asked **Start chatting with your team**, choose **Start a group chat with everyone**, **Create a 1:1 chat with each member**, or both, then click **Start**. **Not now** skips this step.

To reuse a setup, click **Save as template** before you create the room. Saved templates appear in your Template Library. Every setting can be changed later in the project's **Folder Settings**.

## Creating a project from the sidebar

1. Click the **New Folder** button at the top of the sidebar.
2. Give the folder a name.
3. Open the folder's menu in the sidebar and choose **Configure…** to open **Folder Settings**.
4. Change **Folder Type** to **Project**.
5. Set the **Goal**: a short statement of what this project is about. Agents see this in their system prompt on every turn.
6. (Optional) Add **Shared Project Documents**: files that agents can read on every turn.
7. Add **Team Members**: pick agent templates, assign aliases, set per-member overrides for provider, model and tools.
8. Tick **Mark as coordinator** on one member.

> **Note**: Project members are stored once at the project level. Every conversation created inside the project inherits the same member roster. Edit a member's provider once; it applies to all conversations under the project.

## How conversations inherit the project

When you put a conversation inside a project folder (right-click a conversation → **Move to Folder…**, or use the chat buttons under the project in the sidebar), the conversation:

- Inherits the project's member roster (visible in **Chat Settings → Group Members**).
- Sees the project's goal text as part of the LLM's system prompt.
- Has the project's documents read on every turn.

Per-member overrides apply across the project. A change to `@Maya`'s model in the project's Folder Settings is reflected in every conversation that uses that member.

## Project documents

A project can attach documents that agents see automatically.

1. Open the project's **Folder Settings**.
2. Under **Shared Project Documents**, click **Upload Document…**.
3. Select a text file (Markdown, source code, notes, etc.). The file is copied into the project's document directory.
4. The document is now part of every agent's context on every turn in every conversation under the project.

> **Warning**: Documents count toward the LLM's context window. Adding very large documents may push earlier messages out of context. Trim documents before attaching when possible.

To remove a document, open **Folder Settings → Shared Project Documents** and click **Remove** next to the document.

## Coordinator gate

In a project, one member is marked as the **coordinator**. The coordinator has special privileges:

- Can call `add_project_member` to add new members at runtime.
- Can call `remove_project_member` to remove members at runtime.
- Typically receives the user's initial nudge (`@Alice, start here`).

> **Note**: A non-coordinator member's `add_project_member` call is rejected; only the coordinator can manage the roster at runtime.

## Heartbeats: scheduled work

A heartbeat lets a project member run on a schedule without you prompting it, for example a daily research summary or a periodic check of a website.

1. Open the project's **Folder Settings** and find **Project Heartbeats**.
2. Click **Add Heartbeat for Member…** and pick the member.
3. Set a **Schedule**: `@hourly`, `@daily at HH:MM`, `@weekly on DAY at HH:MM`, or `@interval N` (every N minutes, at least 5).
4. Write a **Goal** (what the member does on each run) and, optionally, **Surface criteria** (when a result is worth sharing; the goal is used if you leave it empty).
5. Set **Max runs/day** and, optionally, a **Post target** conversation. With no target the reports stay in the heartbeat overlay.
6. Click **Add Heartbeat**.

Reports always go to the heartbeat overlay first. A report is posted into a chat only if that chat has **Allow auto-posted heartbeat reports** turned on in **Chat Settings → Heartbeat**. This is off by default and limited to one post per day unless you raise **Daily cap (auto-posts per day)**. **Run now** starts a run immediately, and **View Heartbeat Activity** in Chat Settings opens the overlay.

In a group chat outside a project, you can give a member a heartbeat when you add it: open **▸ Configure heartbeat for this member (advanced)** in the **Add Member** dialog.

If you tick **Allow agent to manage its own heartbeat config**, the member can change its own goal, schedule and surface criteria, and turn itself on or off. The schedule format and the runs-per-day cap still apply. Every change is recorded in **Settings → Diagnostics → Self-config Audit**. **Settings → Diagnostics → Heartbeat** lists the next scheduled runs and recent runs, and has a **Pause All Heartbeats** button.

Heartbeats cannot use the built-in llama.cpp engine. Give heartbeat members a different provider.

## Activity timeline (project-scoped)

The project's full activity log is reachable from **Folder Settings → Project Activity → View Activity Log**.

The timeline shows every event scoped to this project across all conversations:

- Member added or removed (with who added it)
- Each agent's turn (with provider, model, tokens and elapsed time)
- Each tool invocation (with arguments and status)
- Each poll opened, vote and close
- Each file written
- Each canvas edited

Filter the timeline by **Actor** (users, agents, clients, system) and **Event**.

See [09-activity-timeline.md](09-activity-timeline.md) for details on the timeline UI.

## Polls in a project

Polls happen inside individual conversations. They are not project-wide. Each conversation has its own poll history. Polls are visible in the conversation's activity timeline.

## Individual direct chats with project members

Inside a project, you can open a 1-on-1 chat with any single member:

1. Expand the project in the sidebar to show its members.
2. Click the chat button next to a member (tooltip **Chat with @Name**).
3. A conversation opens inside the project with you and that member only.

The member uses its project-level overrides (same model and tools as in the team chat).

## Where projects are stored

Projects, members and document records are stored in the local database. Document files are copied into the project's folder inside the data folder.

Everything stays on this computer, except what is sent to each member's model provider.

## What's next

- [04-multi-agent-teams.md](04-multi-agent-teams.md): Member overrides and cascade mechanics in depth.
- [09-activity-timeline.md](09-activity-timeline.md): Opening the project audit log.
- [06-canvas-and-tasks.md](06-canvas-and-tasks.md): Canvas and tasks within a project.
