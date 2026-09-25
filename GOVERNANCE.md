<!--
SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
SPDX-License-Identifier: LGPL-3.0-or-later
-->

# Governance

This document describes how Verzeta Studio is governed: who decides what gets merged, how that decision is made, how disputes are resolved, and how governance itself can evolve.

It is written for the project's current size and stage: a small open-source project with one primary maintainer and an early community. The governance model will likely grow as the project does, and this document is the place that growth is recorded.

---

## Current model: Maintainer-led

Verzeta Studio is currently governed by a single **Project Owner / Lead Maintainer**, Aditya Mehra (`hello@verzeta.com`).

The Lead Maintainer has final authority on:

- What changes are merged into the canonical repository.
- What direction the project takes (roadmap, feature priorities, deprecation decisions).
- Who is granted commit access, reviewer roles, or other elevated permissions on the repository.
- License interpretation and enforcement (in coordination with the branding and CLA policies).
- Release timing and version numbering.
- Acceptance and rejection of new dependencies in the link graph.

This is the **Benevolent Dictator For Life (BDFL)** model. It is appropriate for the project's current scale: one primary author, a focused vision, and a relatively small contributor base. It also matches what users / integrators actually want to know: there is a single point of contact and a single source of decisions.

---

## How decisions are made

In practice:

1. **Routine changes** (bug fixes, small features, documentation updates, refactors that stay within an architectural commitment) are reviewed by the Lead Maintainer when time allows, and merged when ready. Review focuses on correctness, tests, license compliance, and alignment with the project's coding standards.

2. **Architectural changes** (changes to the public API surface, new dependencies, changes to the storage schema, changes to the wire protocol, changes to the trust model) start as a discussion issue before any patch is written. The discussion captures the design rationale; the Lead Maintainer makes the final call on whether to proceed. The design discussion stays in the issue for later reference.

3. **License / governance / branding / CLA changes** are made by the Lead Maintainer with notice in the relevant document's commit message. Material changes may also be noted in the release notes. They do not take effect retroactively on already-distributed releases or already-accepted contributions.

4. **Security reports** are handled as described in [SECURITY.md](SECURITY.md). The Lead Maintainer is the initial point of contact for security reports.

5. **Code of Conduct enforcement** follows the process in [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md). The Lead Maintainer is the enforcement contact.

---

## How the maintainer is held accountable

A BDFL model concentrates decision authority. The project's safeguards against that concentration are:

- **The codebase is open source.** Any contributor who disagrees with a direction may fork. The Lead Maintainer cannot prevent a fork (the license guarantees the right), and a healthier project elsewhere is a real possibility a BDFL must respect.

- **The governance document is public** and changes go through Git history. There is no hidden process.

- **The CLA assigns copyright, but is non-exclusive in the direction the contributor cares about.** Under the CLA the contributor assigns the copyright in their contribution to the Project Owner (a full assignment, not a partial or "as-integrated" one; see [CLA.md](CLA.md) §2); in return they retain a non-exclusive right to use their own contribution in their own separate work under any license they choose. The assignment is what lets the contribution be carried across the project's license tracks; the retained non-exclusive right is what ensures the contributor never loses the ability to use their own code.

- **The CoC and the security policy bind the Lead Maintainer as a participant**, not just other contributors. [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md) and [SECURITY.md](SECURITY.md) apply to the Lead Maintainer in the same way as to anyone else.

- **No silent licensing changes.** Per the CLA, contributions previously accepted cannot be retroactively pulled into a more restrictive license; the version of the CLA in effect at acceptance time governs that contribution.

---

## How governance evolves

The current BDFL model is appropriate for the project's current size. As the project grows, governance is expected to evolve. The Lead Maintainer intends to revisit this document when any of the following happens:

- **Five or more sustained reviewers / committers** other than the Lead Maintainer.
- **A corporate contributor at scale** (an organisation submitting through the CCLA with multiple designated contributors over a sustained period).
- **A second core maintainer** has been informally acting in that role and the relationship is ready to be formalised.
- **A community request** signed by at least ten active contributors asking for the governance model to evolve.

The most likely next step is a **Maintainer Team** model: the Lead Maintainer plus a small number (typically 2–4) of additional Maintainers, all of whom have commit access and shared responsibility for routine reviews. The Lead Maintainer retains final authority on architectural and licensing decisions; the broader Maintainer Team handles day-to-day reviews and triage.

Beyond Maintainer Team, foundation-style governance (a steering committee, a separate board, etc.) is **explicitly out of scope** for now. If the project ever grows to that size, the choice between foundation models (LF / Apache / SFC / Eclipse / etc.) is a deliberate decision worth its own discussion.

---

## What we will NOT change without community discussion

Even under the BDFL model, the Lead Maintainer will not change the following without a public discussion first:

- **No new telemetry or analytics** without an explicit opt-in design discussed in public first. The no-telemetry posture in [PRIVACY.md](PRIVACY.md) is a foundational property of the project.

- **No retroactive license changes** on already-accepted contributions or already-distributed releases. The CLA governs this in the contributor direction; this is the governance counterpart in the project-owner direction.

- **No removal of the open-source license tracks** while maintaining the project as open source. The triple-license arrangement (GPL / LGPL / Commercial; see [LICENSING.md](LICENSING.md)) may evolve; what won't change is the existence of at least one OSI-approved license track. Verzeta will not become source-available-only or proprietary-only while keeping the same project identity.

- **No dependency on a single closed-source service** for the project to function. The application must remain runnable with at least one fully-local LLM provider configured (today: Ollama, or the llama.cpp engine in the Local AI edition).

These rules bind the Lead Maintainer in the same way the CLA binds contributors. Removing any of them requires a public design discussion before the change ships.

---

## Conflict of interest

The Lead Maintainer is also the holder of the commercial-license track for Verzeta (see [LICENSING.md](LICENSING.md)). There is no separate corporate entity; commercial-license inquiries are handled by the same individual who makes merge decisions on the codebase.

This creates a structural alignment-of-incentive between "what's good for the commercial track" and "what's good for the open-source project". The Lead Maintainer will:

- Never reject a community contribution because it would compete with a commercial-license deal.
- Never hold back a security fix for the sake of a commercial-license customer.
- Disclose any commercial conflict on a specific decision when one exists.
- Treat the commercial-license terms as completely separate from the open-source license terms; what one community member receives under GPL / LGPL is the same as what every other community member receives.

If you suspect this has been broken in a specific decision, the same email channel used for security reports is the place to raise the concern. Response times vary.

---

## Jurisdiction

For governance and CLA purposes, the governing jurisdiction is the one in which the project owner is registered, per the choice-of-law clause in [CLA.md](CLA.md) §10. Disputes that reach a legal forum are subject to the courts of that jurisdiction.

The project owner's registered jurisdiction may change over time; the governing jurisdiction is always the project owner's registered jurisdiction at the time of dispute. Material changes may also be noted in the release notes.

---

## Contact

- **General governance questions**: open an issue on the project's repository.
- **Confidential governance questions** (conflicts of interest, accountability concerns, governance complaints): write to the address in [SECURITY.md](SECURITY.md) with the subject prefix `[Verzeta Governance]`.
- **Commercial-license inquiries**: subject prefix `[Verzeta Commercial]`.
- **Trademark inquiries**: subject prefix `[Verzeta Trademark]`.
- **CLA inquiries**: subject prefix `[Verzeta CLA]`.

---

## Document maintenance

This document is part of Verzeta Studio's source tree. Material changes (governance-model evolution, jurisdictional shifts, conflict-of-interest disclosures) are made via Git commits with explanatory messages, and may also be noted in the release notes.

Last reviewed: 2026-05.
