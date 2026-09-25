<!--
SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
SPDX-License-Identifier: LGPL-3.0-or-later
-->

# Branding & Attribution

**Verzeta™** is the name of this project. The project's creator asserts a
common-law claim on the name through ongoing use; no formal trademark
registration is currently in place. This document is a plain-English
guide to how the Verzeta name and logo should be used by people writing
about, contributing to, or building on the project.

It is **not** a restriction on what you may do with the source code.
Forking and modification rights come from the licenses (GPL-3.0-or-later,
LGPL-3.0-or-later, or commercial; see [LICENSING.md](LICENSING.md)) and
are fully intact. This document only addresses how derivative work
should be _named_ and _branded_ so the canonical project stays
identifiable.

---

## What you may do without asking

- **Refer to Verzeta by name** in articles, blog posts, tutorials,
  reviews (favourable or critical), comparison tables, conference
  talks, documentation, and academic publications. Use the name
  accurately.
- **Link to the Verzeta website, repository, or releases** with the
  name as the link text or on a button or badge.
- **Distribute unmodified copies** of Verzeta under its open-source
  license, keeping the name and logo intact.
- **Discuss your customisations** ("I'm running Verzeta with a custom
  skill" / "I added an Ollama provider to my Verzeta install" / "I
  forked Verzeta to experiment with X"). Factual statements about
  what you've done are always fine.
- **Write a plugin, skill, or downstream tool** that interoperates with
  Verzeta and name it descriptively: "Foo for Verzeta", "Bar, a
  Verzeta plugin", "Baz, compatible with Verzeta". The qualifying word
  is what makes the use a clear reference rather than a brand
  appropriation.
- **Use a small "Built with Verzeta" / "Compatible with Verzeta" badge**
  on your own project's README or marketing page, linking back to the
  canonical project.

The throughline: refer to the project accurately, don't pretend to be
the project, don't pretend an endorsement that doesn't exist.

---

## What suggests an official endorsement (please don't)

The following uses would lead a reasonable reader to think your work
is, or is endorsed by, the canonical Verzeta project. Please pick a
different approach:

- **Publishing a fork or modified distribution under the unmodified
  name Verzeta**, including a fork that ships with "Verzeta" on its
  app icon, package name, installer title, store listing, or release
  artifacts. Pick a different name for your fork.
- **Naming a product or service "Verzeta-something"**: "VerzetaPro",
  "Verzeta Cloud", "Verzeta Enterprise", "Hosted Verzeta",
  "Verzeta-as-a-service" all read as official Verzeta offerings even
  when they aren't.
- **Registering a domain, social-media handle, app-store listing, or
  organisation name containing "verzeta"** for a derivative product,
  service, or distribution, including obvious misspellings
  (verzetta, verzetaopen, verzeta-pro).
- **Using the Verzeta logo** (the stylised "V" image distributed under
  `resources/icons/`) as the primary identity of your own product.
- **Claiming endorsement, sponsorship, certification, partnership,
  approval, or affiliation** with the Verzeta project when none exists.

These aren't legal threats. This is a request for honest naming. The
goal is just to keep "Verzeta" pointing at the canonical project, so
people know what they're getting.

---

## Naming your fork

If you're forking and want to redistribute, three patterns work well:

| Pattern                      | Example                             | Notes                                                                                          |
| ---------------------------- | ----------------------------------- | ---------------------------------------------------------------------------------------------- |
| A wholly new name            | "Quasar Studio", "Lumenpad", "Coil" | Cleanest. State "originally derived from Verzeta" in the README to give credit.                |
| Descriptive + your own brand | "Acme Workspace (Verzeta-derived)"  | Put "Verzeta-derived" as a parenthetical or separate sentence, not in the product name itself. |
| Functional descriptor        | "Local Multi-Agent Chat"            | Works well when you want to emphasise what your fork does rather than its origin.              |

What doesn't work: "VerzetaPro", "Verzeta-Plus", "Cloud Verzeta",
"Verzeta but for Slack" (anything that puts the Verzeta name in the
product name itself).

If you're not sure about a name, the safer path is to ask. Write to
the address in [SECURITY.md](SECURITY.md) with the subject prefix
`[Verzeta name]` and a one-paragraph description of what you're
building.

---

## Other names mentioned in our docs

Names like Qt, Kirigami, KDE Frameworks, Android, Ollama, OpenAI,
Anthropic, Google, Gemini, llama.cpp, and any other third-party
project referenced in Verzeta documentation belong to their
respective owners. We refer to them descriptively; ownership stays
with the original.

Likewise, contributors retain all rights in their own names and
marks. The CLA assigns copyright in code contributions only, not
personal identity.

---

## Document maintenance

This document is part of the Verzeta source tree. Material changes are
made via Git commits with explanatory messages. The Git history is the
canonical record.
