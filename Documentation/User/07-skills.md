# Skills

A skill is an installable instruction pack that an agent can discover and apply. Skills add knowledge or step-by-step instructions to agents without changing their system prompts. Skills never grant tools and never run scripts on their own.

## What is a skill?

A skill is a folder with:

- A **`SKILL.md`** file that starts with YAML frontmatter (`name`, `description`, and optional `tags`, `version` and `tools`), followed by the instructions.
- (Optional) Additional supporting files the skill references: examples, templates, reference docs.

Example minimal `SKILL.md`:

```markdown
---
name: cli-quickstart
description: How to scaffold a small Python CLI app using typer or click.
tags: [python, cli]
---

# CLI Quickstart

When the user asks to design or scaffold a Python CLI:

1. Choose `typer` for new code (type-hint native).
2. Define one entry function per subcommand.
3. Persist to JSON, one record per file under `~/.<app>/`.
```

The agent reads this skill when relevant and follows the instructions inside.

## Installing skills

You can install skills from two sources:

### 1. ClawHub (the public skill catalog)

ClawHub is a public skill catalog. To install from it:

1. Open **Skills** in the navigation bar.
2. Open the **Search & Install** tab.
3. Search by name or topic and press Enter.
4. Click **Install**. Items the catalog flags as malware cannot be installed; suspicious items show the catalog's verdict.
5. Review and approve the skill (see below).

### 2. A local folder (skills you write yourself or get from a colleague)

1. Open **Skills** in the navigation bar.
2. Click **Import Folder** and select the skill's folder. Verzeta copies it into its own skills folder; your original folder is not changed.
3. Review and approve the skill (see below).

> **Warning**: Skills are **user-approved** before agents can use them. Every new skill starts as **Unreviewed**, and agents cannot use it until you approve it. You can also **Block** a skill to keep it unusable. This is the security boundary: Verzeta will not silently follow instructions from a skill you have not reviewed.

## Approving a skill

1. Open **Skills** and go to the **Review Queue** tab, or find the skill under **Installed**.
2. Click **Review & approve** to open the skill's instructions, frontmatter and any warnings.
3. Read it. Tick **I understand that I am responsible for how this skill is used.**
4. Click **Approve**, or **Block** to reject it.

The skill moves to the approved state. Agents can now discover and use it.

> **Pro-tip**: Treat skill approval the same way you treat installing a new browser extension. Read what it does before you click Approve.

## How agents discover skills

If a chat or its project has preferred skills, each request lists them with a one-line description. Skills that declare the tools they need are tagged:

- **`[READY]`**: every tool the skill declares is available in this chat.
- **`[BLOCKED: missing ...]`**: one or more declared tools are not available here, for example because tools are turned off.

If there is no preferred list, agents are told to call `discover_skills` to see the approved skills.

The agent can then call:

- **`discover_skills`** to list the approved skills available in this chat.
- **`read_skill(skill_id)`** to read a skill's full body.
- **`read_skill_file(skill_id, filename)`** to read a supporting file inside the skill folder.

The agent uses what it learns to drive its next reply.

## Preferred skills: pointing agents at the right skills

Every approved skill is available in every conversation through `discover_skills`. To make agents use particular skills, set a preferred list:

- **For a project or organization**: **Folder Settings → Preferred Skills…**
- **For one conversation**: **Chat Settings → Preferred Skills…**. In a project chat, tick **Override Project / Organization Preferred Skills List** to use a different list.

Preferred skills are listed at the start of every request, in the order you set. Tick **Use / Expose Only Preferred Skills** to hide all other skills from agents in that scope.

## Skill order matters

Preferred skills appear in the order you set, with ready skills before blocked ones. If two skills conflict, put the one you want followed first.

## Removing a skill

On the **Skills** page, find the skill and click **Remove skill**. This deletes the skill's files and removes it from every preferred list. To use it again you must install it again.

## Authoring a skill

To create a skill of your own:

1. Make a folder named after the skill (e.g. `cli-quickstart/`).
2. Inside, create `SKILL.md` with YAML frontmatter (`name`, `description`, optional `tags` and `tools`) and the instructions in the body. Verzeta builds the skill's id from the `name` (or the folder name), using lowercase letters, digits, hyphens and underscores.
3. (Optional) Add other files (templates, examples, reference docs).
4. On the **Skills** page, click **Import Folder** and select it.
5. Review and approve it.

> **Pro-tip**: Keep the description to one clear line, because that is what agents see in every request. Keep the body short too: an agent reads the whole body when it opens the skill, and it counts against the context window.

## What's next

- [04-multi-agent-teams.md](04-multi-agent-teams.md): Skills in a team setting.
- [09-activity-timeline.md](09-activity-timeline.md): Auditing what the agents did.
