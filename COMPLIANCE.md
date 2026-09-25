<!--
SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
SPDX-License-Identifier: LGPL-3.0-or-later
-->

# Compliance Posture

This document records Verzeta Studio's compliance posture with respect to common regulatory frameworks. It is written to give users, IT departments, and legal reviewers a single citable source describing what the application actually does and does not do, and what compliance obligations rest with whom.

Verzeta Studio is open-source software distributed under a triple-license arrangement: GPL-3.0-or-later for the differentiated components, LGPL-3.0-or-later for supporting infrastructure, and commercial licenses for organisations that do not want either copyleft variant. The full classification and rationale are in [LICENSING.md](LICENSING.md). For the compliance analysis below, what matters is that the publisher distributes the software and the user installs and operates it on their own hardware. This separation matters for every framework below.

For the user-facing version of the same information in less formal language, see [PRIVACY.md](PRIVACY.md).

---

## Architectural baseline (relevant to all frameworks)

The following statements are verified against the source code:

- **Local-first storage.** All conversations, attachments, messages, projects, polls, skills, and activity logs are stored in a SQLite database on the user's own device (`~/.local/share/Verzeta/verzeta-studio/` on Linux, `%APPDATA%\Verzeta\verzeta-studio\` on Windows). The database file is owned by the user account that runs the application.

- **No telemetry.** The application does not collect, transmit, or aggregate usage analytics. There are no developer-operated servers receiving data of any kind.

- **No cloud sync.** Conversations and settings do not synchronise to any cloud service.

- **No developer access.** The publisher has no access, technical or contractual, to any user's data. There is no shared backend, no central service account, no remote administration channel.

- **Outbound network only to services the user sets up.**
  - LLM provider APIs (OpenAI, Anthropic, Google Gemini, OpenRouter, DeepSeek, Ollama, llama.cpp Remote) are contacted only when the user has explicitly configured a provider with an API key or URL and sends a message addressed to that provider. The content of those requests goes to that provider per the provider's own terms.
  - The optional remote-pairing daemon (`verzeta-remote`) accepts WebSocket connections from devices the user has explicitly paired. Network reachability is the user's responsibility.
  - When the user sets them up, the application also contacts the configured web search backend, image and embeddings providers, MCP servers, ClawHub (skill search and download) and Hugging Face (model and voice downloads). [PRIVACY.md](PRIVACY.md) lists each one.
  - No other outbound traffic.

- **API key storage.** Provider API keys are stored locally, obfuscated (not encrypted) in the local settings file. Keys never leave the user's device except as bearer credentials in requests to the matching provider.

- **Open source.** The full source code is browsable, modifiable, and auditable under the triple-license arrangement described in [LICENSING.md](LICENSING.md). Compliance reviewers can verify every claim in this document against the source.

---

## PCI DSS (Payment Card Industry Data Security Standard)

**Status: NOT APPLICABLE.**

Verzeta Studio does not store, process, or transmit cardholder data. The application has no payment surface. Users pay LLM providers (OpenAI, Anthropic, etc.) through each provider's own billing system, which is operated entirely outside Verzeta Studio. The application never sees a credit card number, a payment token, or any cardholder data.

**Compliance obligations on the publisher**: none.

**Compliance obligations on the user**: none with respect to Verzeta Studio. If a user pays an LLM provider for API access, that user's relationship with the provider is independent of this software.

---

## HIPAA (Health Insurance Portability and Accountability Act, United States)

**Status: NOT DIRECTLY APPLICABLE to the distributed software.**

HIPAA imposes obligations on "covered entities" (US healthcare providers, health plans, healthcare clearinghouses) and their "business associates" (third parties handling Protected Health Information on a covered entity's behalf). The publisher of Verzeta Studio is neither. HIPAA does not impose obligations on an open-source software publisher distributing a local-first tool.

### User responsibility (if you are a covered entity)

If you are a covered entity and you plan to use Verzeta Studio in connection with PHI, the compliance obligations rest entirely with you, and they depend on which providers you configure:

- **Fully local providers (Ollama, the built-in llama.cpp engine).** PHI sent to an agent backed by a local provider does not leave your machine. You retain full custody of the data and full HIPAA responsibility for it. Verzeta does not need to be a business associate because the data does not leave your environment.

- **Cloud LLM providers (OpenAI, Anthropic, Google Gemini, OpenRouter, DeepSeek, or any remote endpoint).** PHI sent to a cloud-backed agent is transmitted to that provider. The provider becomes a data recipient. You must have a Business Associate Agreement in place with the provider before sending PHI. Whether that BAA exists is between you and the provider; Verzeta cannot mediate, vouch for, or certify it.

- **Remote-paired clients (Android, etc.).** A paired remote client transmits PHI between your phone and your desktop over a WebSocket connection (encrypted with TLS when you enable it). Both endpoints are under your control; both are part of your HIPAA-covered environment.

### Recommended pattern for HIPAA-covered users

The lowest-risk configuration is to use Verzeta Studio with only local providers (Ollama or the built-in llama.cpp engine) and to disable cloud providers in **Settings → Text Providers**. In this configuration no PHI ever leaves your device.

---

## GDPR (General Data Protection Regulation, European Union)

**Status: the user is the data controller; the publisher has no controller or processor relationship with end users.**

### Why the architecture is GDPR-friendly

GDPR applies to entities that determine the purposes and means of processing personal data of EU residents. For Verzeta Studio:

- The user installs the software on their own device.
- The user determines which conversations to have and which providers to send them to.
- The data lives in a SQLite database the user controls.
- The publisher never receives a copy of any user's data.

Under GDPR vocabulary, the user is the **data controller** for their own conversations. The publisher of Verzeta Studio is neither a controller nor a processor of user data. There is no relationship to be a controller or processor in.

### Data subject rights

GDPR grants individuals several rights over their personal data. Verzeta Studio supports each by design:

| Right                                                 | How it is satisfied                                                                                                                                                                                                                             |
| ----------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Right of access (Article 15)                          | Users can browse all conversations and messages in the app. They can also export individual conversations to Markdown or JSON via **Conversation menu → Export**. The underlying SQLite file is also directly readable with any SQLite browser. |
| Right to rectification (Article 16)                   | Users can edit their own messages in the chat. They can also manually edit the SQLite database.                                                                                                                                                 |
| Right to erasure (Article 17)                         | Users can delete individual conversations, individual messages, or the entire app-data folder. There is no soft delete and no remote copy to coordinate with.                                                                                   |
| Right to data portability (Article 20)                | The SQLite database is in a documented, open format. Export to JSON / Markdown is built in.                                                                                                                                                     |
| Right to restrict processing (Article 18)             | Users can disable providers in **Settings → Text Providers** or simply not send the affected conversations to any provider.                                                                                                                     |
| Right to object (Article 21)                          | No automated decision-making is performed on the user's behalf by the publisher.                                                                                                                                                                |
| Rights against automated decision-making (Article 22) | Decisions made by AI agents inside the app are local, user-driven, and subject to the user's review and intervention at every step.                                                                                                             |

### What happens when data is transmitted to a cloud provider

When the user sends a message to a cloud LLM provider, that provider receives the message content per its own privacy policy and terms of service. That data exchange is between the user and the provider; the publisher of Verzeta Studio is not in the middle.

Cloud providers have their own GDPR status and obligations. Their privacy policies, processor agreements (DPAs), and lawful bases for processing apply directly to the user-to-provider relationship.

### Remote pairing and GDPR

When a user pairs an Android device with their desktop:

- The desktop and the phone are both endpoints under the same user's control.
- The WebSocket between them is encrypted when TLS is enabled (recommended).
- No third party (including the publisher) sits on or has access to that connection.

This is comparable to any other LAN file-sync between the user's own devices.

### What the publisher does not have

- No central database of users.
- No analytics or telemetry collection.
- No account system (Verzeta does not require sign-in).
- No "delete my account" obligation, because no account exists.
- No data-subject-request workflow, because no data-subject data exists in publisher-controlled storage.

---

## Other frameworks (brief)

### CCPA / CPRA (California Consumer Privacy Act, USA)

Same analysis as GDPR: the publisher does not sell, share, or hold personal information about users. The application is local-first; the user controls their own data.

### Children's Online Privacy Protection (COPPA, USA)

Verzeta Studio does not directly collect data from any user. It does not target children. Users responsible for minors should evaluate the LLM providers they configure against COPPA requirements.

### Accessibility (Section 508, EAA, WCAG)

Verzeta Studio is built on Qt 6 and KDE Kirigami, which provide standard accessibility primitives: screen-reader compatibility, keyboard navigation, theme contrast. The application honours the underlying frameworks' accessibility surface.

---

## Summary table

| Framework       | Applicable?             | Publisher obligations          | User obligations                                                                             |
| --------------- | ----------------------- | ------------------------------ | -------------------------------------------------------------------------------------------- |
| **PCI DSS**     | No                      | None                           | None (no payment data)                                                                       |
| **HIPAA**       | Not directly            | None                           | If a covered entity, manage BAAs with cloud providers; consider local-only providers         |
| **GDPR**        | User is data controller | None (publisher holds no data) | Manage their own controller obligations; manage processor relationships with cloud providers |
| **CCPA / CPRA** | User is responsible     | None                           | Same as GDPR                                                                                 |
| **COPPA**       | Not directly            | None                           | Evaluate providers against COPPA if minors involved                                          |

---

## How to verify these claims

This document makes specific claims about what the software does and does not do. Every claim is verifiable in the open-source code:

- **No telemetry.** No outbound network calls are made except to the providers and services the user has set up (listed above and in [PRIVACY.md](PRIVACY.md)), and to paired remote-access clients the user has explicitly authorised.
- **Local-first storage.** The application's data folder is created under the operating system's standard application-data location and stays there.
- **API key handling.** Keys are stored locally, obfuscated in the local settings file. They never leave the device except as the bearer credential in a request to the matching provider.
- **No account / sign-in.** There is no account model, no central authentication service, and no publisher-controlled login.
- **Pairing scope.** The optional remote-pairing daemon accepts WebSocket connections only from devices the user has paired with a one-time code, using bearer tokens stored on the device.

If you find a discrepancy between this document and the application's behaviour, please open an issue on the project's Git repository. A disagreement between documentation and behaviour is treated as a bug.

---

## Document maintenance

This document is part of Verzeta Studio's source tree. It is updated as needed when a change to the application meaningfully affects any of the claims above (e.g. if telemetry were ever added, this document would have to change first).

Last reviewed: 2026-05.
