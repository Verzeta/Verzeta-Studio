# Agent execution and permissions

Some agent tools run real programs on your computer. The main one is the
shell tool, which lets an agent run command-line programs, for example to
install a project's dependencies, run a build, or start a local development
server for something it just wrote.

Because these commands run on your machine, Verzeta only lets agents run
programs you have allowed. This page explains that allow-list, how to change
it, and the other kinds of tools agents can use.

Each command may run for up to two minutes. Longer programs, such as a
development server, can be started in the background; up to four can run at
once.

## The shell command allow-list

Verzeta keeps a list of the programs an agent is allowed to run. When an agent
tries to run a command, Verzeta looks at the first word (the program name). If
it is on the list, the command runs; if not, the command is refused and the
agent is told the program is not allowed.

Out of the box the list on Linux already contains the everyday tools a coding
agent needs, including:

- File inspection tools such as `ls`, `cat`, `grep`, `find`.
- Language runtimes and package managers such as `python`, `node`, `npm`, `pip`.
- Version control and build tools such as `git`, `make`, `cmake`.
- Network fetch tools `curl` and `wget` (useful for checking a local server an
  agent has started).
- `kill` and `pkill`, to stop a background process.

On Windows the default list uses the Windows equivalents instead, such as
`dir`, `type`, `findstr` and `where`, plus `python`, `git`, `cmake`, `ninja`
and `curl`. Add others such as `node` or `npm` yourself.

## Changing the list

Open **Settings** and find the **Execution & Permissions** card, then select it
to open the editor. There you can:

- **See every allowed program** as a chip.
- **Add a program** by typing its name and choosing **Add**.
- **Remove a program** by selecting the remove button on its chip. You can
  remove any program, including one that came as a default.
- **Restore to Default** to bring the full built-in list back, including
  anything you removed.

Changes take effect immediately and apply to every agent and every
conversation.

## Safety

You are responsible for the programs you allow. An agent can run any allowed
program, so only add tools you are comfortable letting an agent use.

Two protections stay in place no matter what is on the list:

- **Dangerous command patterns are always blocked.** For example, deleting
  files recursively (`rm -r`), running commands as a superuser (`sudo`),
  formatting a disk, writing to system paths, shutting down the computer, or
  downloading a script and piping it straight into a shell are refused even if
  the program itself is allowed.
- **Commands start in the current project's folder.** This is where relative
  paths point. It is not a sandbox: a command can still use absolute paths or
  `cd` elsewhere, with your user's permissions. Unlike canvas **Run**, agent
  shell commands are not run inside bubblewrap.

If you remove every program from the list, agents will not be able to run any
shell command until you add one back or choose Restore to Default.

## Built-in tools, custom tools and MCP servers

Open **Tools** in the navigation bar to see every tool agents can use. Each
tool has a switch to turn it on or off.

- **Built-in**: the tools that ship with Verzeta Studio.
- **Custom**: your own tools, added with **Add Custom Tool**. Each one runs a
  **Command Template**, with `{{param_name}}` replaced by the value the agent
  passes. Custom tools do **not** go through the allow-list or the
  dangerous-pattern check: the command runs directly in a shell with your
  user's permissions, for up to 30 seconds. Write the template with that in
  mind.
- **MCP**: tools from Model Context Protocol servers. Click **Add MCP Server**,
  choose the **Transport Type** (stdio for a local program such as `npx`,
  `python` or `uvx`, or SSE or Streamable HTTP for a server), and enter the
  command or URL. Once the server connects, its tools
  appear as `server:tool` and can be turned on or off one by one.

Type `/showtools` or `/showmcptools` in a chat to see which tools that chat
can use. To limit the tools of one team member, use its tool whitelist (see
[04-multi-agent-teams.md](04-multi-agent-teams.md)).
