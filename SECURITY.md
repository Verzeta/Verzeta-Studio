<!--
SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
SPDX-License-Identifier: LGPL-3.0-or-later
-->

# Security Policy

This document describes how to report security vulnerabilities in Verzeta Studio and what to expect after a report is filed. Verzeta is a local-first, open-source desktop application with mobile and editor clients.

If you have found a vulnerability, please follow the reporting channels below rather than opening a public issue. Public disclosure of an unpatched issue puts every user at risk.

---

## What is in scope

This policy covers the Verzeta Studio desktop application, its optional remote-pairing daemon, and the Verzeta clients for Android and VS Code.

## What is not in scope

The following are by design not vulnerabilities in Verzeta Studio. Reports about them will be closed with a pointer here:

- **An LLM provider mishandling content you sent to it.** Each provider has its own privacy policy and terms. Verzeta does not log or proxy data sent to providers (see [PRIVACY.md](PRIVACY.md)). Concerns about a provider's behaviour belong with that provider.
- **A user choosing to paste an API key into a screenshot, chat with another person, or otherwise distribute it themselves.** Verzeta does not exfiltrate keys; users distributing their own credentials voluntarily is outside the application's perimeter.
- **An attacker with full local access to a logged-in user account.** Verzeta stores data in the user's app-data folder. An attacker who is already the local user has access to that folder by definition. Threat models requiring full-disk encryption or OS-level access controls are out of scope for an application-level policy.
- **An LLM producing harmful, biased, or false content.** This is a property of the model, not a vulnerability in Verzeta. Report content-policy violations to the provider.
- **An open-source supply-chain risk in an upstream dependency.** Please report it to the dependency's maintainers.

---

## How to report

Please use **one** of the following channels. Email is preferred for initial contact.

### Email (preferred)

Send an email to **<hello@verzeta.com>** with the subject prefix **`[Verzeta Security]`**.

Include in the email:

- **What you found.** A short description in your own words.
- **Where.** The file path, function name, or feature where you observed the issue.
- **How to reproduce.** Step-by-step instructions, ideally with a minimal command sequence or attached patch.
- **Impact.** What an attacker could do with this; under what threat model.
- **The version.** Either the commit hash, the release tag, or the build's About-dialog version string. We need to confirm we are looking at the same code.
- **How you would like to be credited** if we publish a note, or "no credit, please."

If we reply, it will be from the same address.

### Git repository's security advisory feature

If the hosting platform supports private security advisories (for example, GitHub Security Advisories), you may also file there. These are visible only to the maintainers.

### Encrypted email (PGP)

We do not publish a PGP key. If you need encrypted contact, ask in your email and we will see what we can arrange.

---

## What to expect after you report

- Reports are reviewed as time allows. Response times vary, and we do not commit to fixed timelines.
- We may contact you if we need more information.
- Whether a report is treated as a vulnerability, and whether and when it is fixed, is at the maintainers' discretion.

## How fixes are released

- Security fixes ship in normal releases. A fix may be released without any public description of the issue.
- Whether an issue receives a release-note entry, an advisory or any other public mention is at the maintainers' discretion.
- We do not run a CVE or advisory process by default.
- If we are unable to fix an issue, we may document the limitation and a workaround.

## Keeping details private

Please keep the details of what you report private, and do not share them with others. If you intend to publish anything about it, tell us first by replying to your report, and give us a reasonable chance to release a fix.

## Credit

If an issue you reported is mentioned publicly, you can ask to be credited.

## Bug bounty

There is no bug bounty, monetary or otherwise.

---

## Rules for testing

This policy does not give anyone permission to test against systems, devices or data that are not their own. If you look for vulnerabilities in Verzeta:

- Test only on your own installation, your own devices and your own data.
- Do not test against Verzeta hosts, paired clients or data that belong to other people, or against the project's website and other infrastructure.
- Do not attack the third-party services the app talks to, such as LLM providers, search providers or model download servers.
- No denial-of-service testing against anything but your own machine, and no social engineering, spam or physical attacks.
- Stop once you have shown the issue. Do not keep, share or use any data you reached.

This policy is not a legal agreement. It does not authorise anything that is otherwise unlawful, and it does not waive any rights.

---

## Document maintenance

This document is part of Verzeta Studio's source tree. Updates are made via Git commits with explanatory messages. The Git history is the canonical record.

If you find that this document is wrong, please tell us through the channels above.

Last reviewed: 2026-09.
