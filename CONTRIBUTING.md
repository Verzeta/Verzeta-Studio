<!--
SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
SPDX-License-Identifier: LGPL-3.0-or-later
-->

# Contributing to Verzeta Studio

Thank you for your interest in contributing to Verzeta Studio. This document explains how to file issues, propose changes, and what is expected when you submit a patch.

Verzeta Studio is released under a triple-license arrangement (GPL-3.0-or-later for differentiated components, LGPL-3.0-or-later for supporting infrastructure, and commercial licenses for organisations that do not want either copyleft variant). See [LICENSING.md](LICENSING.md) for the per-component breakdown. Contributions of all sizes are welcome: bug fixes, new features, documentation, tests, translations and feedback.

---

## Quick links

- **Found a bug?** Open an issue. See [Reporting bugs](#reporting-bugs) below.
- **Have an idea?** Open a feature-request issue first, before writing a patch. See [Proposing features](#proposing-features).
- **Found a security vulnerability?** Do **not** open a public issue. See [SECURITY.md](SECURITY.md) for responsible disclosure.
- **Want to translate?** See [Translations](#translations).
- **Ready to submit a patch?** See [Submitting changes](#submitting-changes).

---

## Code of Conduct

We follow a written code of conduct: see [CODE_OF_CONDUCT.md](CODE_OF_CONDUCT.md). Everyone participating in the project's spaces (issues, merge requests, discussions and documentation) is expected to follow it. If you encounter behaviour that violates the code, please report it via the channels in that document.

---

## Reporting bugs

A useful bug report contains four parts:

1. **What you did.** Step-by-step instructions, including the operating system, the version of Verzeta Studio (shown in **About** on the Home page), and which providers were configured.
2. **What you expected to happen.**
3. **What happened instead.** Including any error messages.
4. **What you have already ruled out** (optional but helpful). "I have already restarted the app and the issue persists" saves a round trip.

If the bug involves a specific LLM provider, include the provider name and (when relevant) the model name. The data-flow details often depend on which provider was active.

> **Privacy note**: please do **not** paste API keys, full conversation contents, or other sensitive data into bug reports. Redact before posting. If a reproducer requires data that you cannot share publicly, say so in the issue.

Logs that may help with diagnosis live at `~/.local/share/Verzeta/verzeta-studio/logs/verzeta-studio.log` (Linux) or `%APPDATA%\Verzeta\verzeta-studio\logs\verzeta-studio.log` (Windows). Attach the relevant section if you are comfortable doing so.

---

## Proposing features

Before writing a patch for a new feature, please open an issue describing what you want to build and why. Reasons:

- The feature may overlap with planned work, and we can save you the duplication.
- The feature may interact with an existing architectural commitment in ways that need design discussion.
- The feature may need design notes the maintainer can share before you start. Significant features generally land best when the contributor and the maintainer agree on the shape first.

A short issue is enough. Response times vary. The reply is usually one of:

- "Great, please go ahead" with any design notes.
- "We are interested but need this to fit alongside X" with the constraint.
- "We do not plan to take this in" with the reason. You are welcome to maintain it as a fork.

---

## Submitting changes

### Branching

The project develops on `main`. Commits land directly on `main` when small and reviewable. Larger or sensitive changes may be developed in topic branches first, but everything that ships is in `main` history.

If your patch is large, open a draft merge request early to get feedback.

### Commit discipline

**Commits are for discrete code changes or completed features.** Do not open a commit purely to cross-link docs or for scratch process notes. Docs-only commits are fine when they fix a real inaccuracy or add real content; what we avoid is no-op churn.

The user-facing documentation under `Documentation/User/` is part of every contribution that changes user-visible behaviour.

Commit messages should follow the project's pattern: a single-line summary, then a body explaining the **why** (not just the what). Look at recent `git log --oneline` entries on `main` for the established style.

When co-authored work is involved, append a `Co-Authored-By:` trailer at the end of the message.

### Code style

- **Languages**: C++20 for the backend, QML (Qt 6) for the frontend, Kotlin for the Android client.
- **Formatting**: `clang-format` for C++ (config in `.clang-format` at the project root) and `qmlformat` for QML. Please run both before you submit.
- **Static analysis**: `clang-tidy` is configured in `.clang-tidy`. Please fix its warnings in the code you change.
- **Naming**: classes are `CamelCase`, methods are `camelCase`, member variables prefix `m_`, constants prefix `k`. Files are `lowercase-with-hyphens`.
- **Comments**: write WHY, not WHAT. Use Doxygen `/** */` blocks for the public API, with `@brief`, `@param` and `@returns`. Keep internal development context (decision dates, issue numbers, iteration markers) in commit messages, not inline narrative.
- **Headers**: every source file needs SPDX metadata: `SPDX-FileCopyrightText` and an `SPDX-License-Identifier` that matches the per-component classification described in [LICENSING.md](LICENSING.md). Run `reuse lint` to verify.

### Testing

Verzeta Studio has three kinds of tests:

- **Unit tests** in `tests/unit/`: fast tests of single components.
- **Integration tests** in `tests/integration/`: fast tests across several services.
- **Stress tests** in `tests/stress/`: run the real backend against live LLM providers. They are not run in CI; the maintainer uses them to catch regressions.

Run the test suite before submitting:

```bash
ctest --test-dir build-debug --output-on-failure
```

**Required**:

- Tests must pass at 100% before merge.
- New features must come with new tests. Patches without tests are returned with a request to add them.
- Bug fixes must come with a regression test that fails without the fix.

The stress harness at `tests/stress/stress-group-chat-multi-provider.cpp` is the regression guard for changes in any of:

- `backend/api/`
- `backend/services/chat/`
- `backend/services/chat-controller.{h,cpp}`
- `backend/services/model-router.{h,cpp}`
- `backend/services/membership-service.{h,cpp}`
- `backend/services/poll-service.{h,cpp}`
- `backend/services/canvas-service.{h,cpp}`
- `backend/tools/`

If your patch touches any of those, the maintainer may run the stress harness before merging. You are welcome to run it locally too. It needs at least one cloud LLM provider.

### Documentation

If your patch changes user-visible behaviour, update the relevant page in [Documentation/User/](Documentation/User/). Release notes for each public release are written by the maintainer at publish time; you do not need to maintain a separate changelog entry in your patch.

If your patch is documentation-only and fixes a real inaccuracy, that is a fine commit on its own. Please note in the message which source-of-truth file the fix is verified against.

### License and CLA

Verzeta Studio uses a triple-license arrangement: GPL-3.0-or-later for differentiated components, LGPL-3.0-or-later for supporting infrastructure, and commercial licenses for organisations that do not want either copyleft variant. The [LICENSING.md](LICENSING.md) document explains which components fall into which tier.

Operating the commercial track requires the project owner to hold the right to relicense contributed code. Contributors therefore sign a Contributor License Agreement that:

- Assigns copyright in the contribution to the project owner.
- Grants a perpetual, irrevocable, royalty-free patent licence (with the standard patent-retaliation clause).
- Warrants that the contribution is the contributor's own work and free of third-party encumbrances.

The full text and the sign-off mechanism are in [CLA.md](CLA.md). Every commit in a pull request also needs a `Signed-off-by:` line (use `git commit -s`). A check on each pull request reports commits that are missing it.

Every file in your contribution must carry the SPDX headers described above. The project is [REUSE 3.0](https://reuse.software/) compliant; run `reuse lint` locally to verify.

---

## Translations

Verzeta Studio uses Qt's `qsTr()` mechanism for translatable strings. All user-visible text in the QML layer is wrapped in `qsTr()`.

If you would like to contribute a translation:

1. Open an issue saying which locale you would like to add.
2. The maintainer can generate the latest `.ts` file from the source for you to translate.
3. Submit the completed `.ts` file as your patch.

There are no translations yet.

---

## Code review

When you submit a patch:

- Reviews happen as time allows.
- Review comments will reference the project's coding standards and architectural commitments. Push-back is welcome; we want the right design more than we want the first design.
- After review, the patch lands on `main`. Squash-merge is the default; the merge commit message comes from your topic branch's commit log.

If your patch has had no response and may have been missed, a follow-up comment on the issue is welcome.

---

## What is NOT a contribution we can take

A few categories of change we will close politely:

- **Telemetry, analytics, fingerprinting, or "anonymous usage statistics".** Verzeta Studio is local-first. See [PRIVACY.md](PRIVACY.md). Having no telemetry is deliberate.
- **Auto-update / phone-home that the user did not explicitly enable.** Same reason.
- **Forced cloud-account requirements.** No user should be required to create an account to use this software.
- **Removing SPDX headers or relicensing components.** The triple-license arrangement is documented in [LICENSING.md](LICENSING.md); per-component classification is intentional.
- **Unrelated changes in one patch.** A patch should do one thing.
- **Style-only changes that touch many files.** These create review burden without functional benefit. If you want to enforce a style rule, propose adding it to `.clang-format` / `.clang-tidy` first.

---

## Questions

- **About the user-facing features**: see [Documentation/User/](Documentation/User/).
- **About the codebase or architecture**: the Doxygen-rendered API documentation is the authoritative public reference; read the relevant source-file headers and class blocks. For higher-level design questions that aren't covered in the source itself, open a discussion issue.
- **About the license arrangement**: see [LICENSING.md](LICENSING.md). Commercial-license inquiries go to the address in [SECURITY.md](SECURITY.md).
- **About privacy or data handling**: see [PRIVACY.md](PRIVACY.md) and [COMPLIANCE.md](COMPLIANCE.md).
- **About security**: see [SECURITY.md](SECURITY.md).
- **About using the "Verzeta" name in your project**: see [TRADEMARKS.md](TRADEMARKS.md).

For anything else, open an issue.

---

Thank you for contributing.
