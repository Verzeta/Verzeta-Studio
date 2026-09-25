<!--
SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
SPDX-License-Identifier: LGPL-3.0-or-later
-->

# Licensing

Verzeta™ Studio is released under a triple-license arrangement. This document explains what that means for users, contributors, integrators, and forks.

For the legal text of each license, see the [`LICENSES/`](LICENSES/) directory. For the source-of-truth per-file declaration, look at the SPDX license-identifier line at the top of each source file. The project is [REUSE 3.0](https://reuse.software/) compliant.

---

## The short version

| You want to…                                                                             | License path                                                                                                                                                                                                                     |
| ---------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Use Verzeta Studio as a desktop / mobile end-user application                            | No action needed. Just install and use.                                                                                                                                                                                          |
| Read, study, modify the source for personal or research use                              | GPL or LGPL. Either covers it.                                                                                                                                                                                                   |
| Fork the project and publish your fork                                                   | Allowed under GPL / LGPL. Your fork must keep the SPDX headers, must redistribute source for any GPL-classified files you ship in binary form, and must not use the "Verzeta" name or logo (see [TRADEMARKS.md](TRADEMARKS.md)). |
| Embed Verzeta's _infrastructure_ code into another open-source project                   | LGPL-classified files only, dynamically linked under LGPL §6.                                                                                                                                                                    |
| Embed Verzeta's GPL-classified code (RAGP, multi-agent cascade, etc.) into a proprietary product | Buy a commercial license. Contact via the email in [SECURITY.md](SECURITY.md).                                                                                                                                                   |
| Use any part of Verzeta in a closed-source product without GPL or LGPL obligations       | Buy a commercial license.                                                                                                                                                                                                        |
| Contribute a patch upstream                                                              | Sign the [CLA.md](CLA.md).                                                                                                                                                                                                       |

---

## The three licenses

### 1. GPL-3.0-or-later: the differentiated components

The files whose SPDX line declares **GPL-3.0-or-later** are the parts of the codebase the project owner considers unique to Verzeta. These include:

- The **RAGP** routing and gating layer: the rule, cache and model-based classifier that decides which agent answers next, together with the shared embedding and retrieval code.
- The **multi-agent cascade**: the per-request model routing, per-member provider overrides, multi-provider cascade orchestration, request-turn alias matching, cross-cascade self-echo window, and turn-completion forcing.
- The **per-client session router**: multi-tenant ChatController lifecycle, cross-instance ModelRouter slot serialization.
- The **streaming / sanitization / tool-dispatch state machine**: cap-hit handling, content rewrite, finalize protocol.
- The **task lifecycle orchestration**: TaskController, TaskRunner, TaskObserver, TaskGateService, PlanService.
- The **heartbeat subagent loop**: configuration, scheduler, report review, and the self-configuration tools.
- The **project / team workflow primitives**: MembershipService coordinator gate, ProjectTemplateService creation pipeline, PollService voting.
- The **unified audit trail**: AuditService recording hooks and cross-event correlation.
- The **canvas runner sandbox**: DangerousPatternScanner, multi-platform `run_shell`, CanvasRunner orchestration.

GPL means: if you distribute a derivative work built on these files in binary form, you must release the corresponding source of your derivative under GPL-3.0 (or a later compatible version). This is the standard "copyleft" obligation.

The canonical license text is [`LICENSES/GPL-3.0-or-later.txt`](LICENSES/GPL-3.0-or-later.txt).

### 2. LGPL-3.0-or-later: the supporting infrastructure

The files whose SPDX line declares **LGPL-3.0-or-later** are the project's _infrastructure tier_: code that is useful to many projects, not just Verzeta. These include:

- All generic models / PODs (`Message`, `Conversation`, `Folder`, …).
- All generic utilities (logger, HTTP client, JSON helpers, crypto helpers).
- All generic CRUD services (ConversationService, MessageService, FileService, SettingsService, …).
- The wire-protocol implementation (the IPC + WebSocket plumbing between desktop and paired clients).
- All UI components and pages (every QML file).
- All LLM provider HTTP wrappers (OpenAI, Anthropic, Gemini, OpenRouter, DeepSeek, Ollama, llama.cpp Remote, and the bridge to the built-in llama.cpp engine).
- Design tokens / theme infrastructure (ThemeController, design-system tokens).
- Build tooling (CMake configuration, CI workflows).
- The entire test suite.

LGPL means: you can dynamic-link these files into a proprietary or differently-licensed work, provided you honour LGPL §6 (allow the user to replace the LGPL'd component, ship the LGPL'd component's source, etc.). This is what makes LGPL "library-friendly" compared to GPL.

The canonical license text is [`LICENSES/LGPL-3.0-or-later.txt`](LICENSES/LGPL-3.0-or-later.txt).

### 3. Commercial licenses: for organisations that do not want copyleft

If neither GPL nor LGPL fits (for example, you want to embed Verzeta's RAGP into a closed-source product, or you need terms different from what GPL §11 patent retaliation offers), the project owner offers custom-negotiated commercial licenses.

Two tiers are anticipated:

| Tier           | What it covers                                                                                                                                       |
| -------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------- |
| **Utils**      | Licenses you out of the LGPL §6 obligations on the LGPL-classified components. For products that only need the infrastructure tier without the GPL components. |
| **Full-stack** | Licenses you out of GPL on the full codebase including the GPL components. For products integrating RAGP / multi-agent workflows directly.                     |

Both tiers include:

- No warranty (standard for open-source-rooted commercial licenses).
- Mandatory attribution (a NOTICES file in your product).
- Use of the "Verzeta" name or logo in your product branding follows the branding and attribution guide in [TRADEMARKS.md](TRADEMARKS.md). A commercial source-code license does not by itself authorise use of the Verzeta name on a derivative product.
- Optional support contracts at additional cost.

Final commercial license terms are individually negotiated. For inquiries, write to the address in [SECURITY.md](SECURITY.md) with the subject prefix `[Verzeta Commercial]`.

---

## What this means in practice

### For end users

You don't need a license to use Verzeta Studio. Install it, run it, share it with friends. All of this is permitted under GPL and LGPL. The triple-license is a developer / integrator concern, not an end-user concern.

### For the binary you download

A built `verzeta-studio` desktop executable contains both GPL-classified and LGPL-classified object code linked into the same process. Under GPL §5 + LGPL §3, the combined binary is **effectively GPL** in shipped form. The Android client is a separate project with its own licensing; see its repository.

The optional `verzeta-remote` wire daemon may be effectively GPL or pure-LGPL depending on which components it links. See the SPDX headers on the daemon's source files for the per-file breakdown.

### For your fork

A community fork is permitted under GPL / LGPL. To stay legal:

- Preserve every SPDX header line at the top of each file.
- If you ship binaries containing any GPL-classified file (almost certainly the case for any working fork), release your derivative's source under GPL-3.0-or-later.
- If you ship binaries containing only LGPL-classified files (rare, since it would be a partial fork), honour LGPL §6: ship object files or dynamic-link points that allow the user to substitute their own version of the LGPL'd component.
- Do not call your fork "Verzeta" or use the Verzeta logo. Pick a different name. See [TRADEMARKS.md](TRADEMARKS.md) for permitted naming conventions.

### For your downstream integration

If you want to ship a product that uses Verzeta code:

- **Pure infrastructure use** (you only need the LGPL'd parts): dynamic-link the LGPL'd source files into your build, honour LGPL §6, and you're done. No commercial license needed.
- **GPL component use** (you need any GPL'd component): either release your product under GPL-3.0-or-later, or buy a Full-Stack commercial license. There is no middle path; that's by design.

The per-file ledger of "what's LGPL vs GPL" is declared inline as SPDX headers and summarised in the project's component-classification record. The same classification appears in the SBOM published with each release.

### For your contribution

To contribute a patch upstream, you sign the [CLA.md](CLA.md). The CLA assigns copyright in your contribution to the project owner. This is what makes the commercial-license track operate: the project owner needs the right to relicense contributed code under commercial terms, and a license-grant-only CLA wouldn't let us do that.

If you prefer to keep your contribution under LGPL or GPL only and not under the commercial track, please do not submit it as a patch. Fork the project, apply your change to your fork, and ship it from there. We're not offended; we cannot take a contribution that splits ownership across the triple-license.

---

## Frequently asked

**Q. Why three licenses instead of one?**

Different parts of the codebase do different work. The wire-protocol code is generic infrastructure useful to many projects; we want the multi-agent code to stay open, so it is under GPL. A single license can't serve both. Infrastructure is LGPL so other projects can use it, the core multi-agent components are GPL, and a commercial license is available for organisations that need different terms.

**Q. Is Verzeta still "open source" if there's a commercial track?**

Yes. Both GPL-3.0-or-later and LGPL-3.0-or-later are OSI-approved. Every file in the repository carries an OSI-approved license declaration. The commercial track is an _additional_ licensing option for organisations that need it. It does not replace the OSI-approved licenses.

This is the same pattern Qt, MySQL, Berkeley DB, and many other widely-adopted open-source projects use.

**Q. Who decides which files are GPL and which are LGPL?**

The per-component classification is recorded in the project's licensing decision document and reflected per-file via SPDX headers. The project owner is the decision-maker for classification changes; PRs proposing reclassification are welcome but go through the normal review.

**Q. Can I see the SPDX headers per file without cloning?**

Once a release is published, GitHub renders SPDX-FileCopyrightText / SPDX-License-Identifier headers natively. The REUSE-3.0 metadata is also machine-readable: `reuse spdx > sbom.spdx` from a checked-out tree produces a full SPDX-format SBOM that lists every file's license.

**Q. What about the dependencies Verzeta links?**

Every direct and transitive dependency was audited as part of the project's pre-release work; the verdict is GPL-compatible (no GPL-only dependencies; LGPL deps comply with LGPL §6 obligations via dynamic linking). The audit document itself is internal, but the SBOM shipped with each release captures the dep graph for downstream packagers + compliance reviewers.

**Q. Why GPL-3.0-or-later specifically, not GPL-2.0 or AGPL?**

GPL-3.0 includes the §11 patent retaliation clause, which is a meaningful defence for a small project. AGPL was considered and rejected. Verzeta is a desktop-first / mobile-paired application, not a SaaS, so AGPL's network-use clause adds enterprise procurement friction without proportional benefit.

The "or later" trailer means you may use any future GPL version (4.0, etc.) when it's published.

---

## What is NOT covered by this document

This document covers Verzeta's _own source code_. It does not cover:

- The trained model weights you connect Verzeta to (those carry their authors' own licenses; check each model's terms).
- The data your conversations generate (that belongs to you and is governed by [PRIVACY.md](PRIVACY.md)).
- Verzeta's name and logo (see [TRADEMARKS.md](TRADEMARKS.md) for the branding and attribution guide).
- Cloud LLM provider APIs you call (each provider's terms apply directly to your traffic with them).

---

## Document maintenance

This document is part of Verzeta Studio's source tree and is updated whenever the licensing arrangement changes. The Git history is the canonical record of every change.

Last reviewed: 2026-09.
