<!--
SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
SPDX-License-Identifier: LGPL-3.0-or-later
-->

# Contributor License Agreement

This document is the Contributor License Agreement (CLA) for Verzeta Studio. Every contributor, individual or corporate, accepts this CLA before their contribution is merged.

The Verzeta CLA is derived from the [Apache Software Foundation Individual Contributor License Agreement v2.0](https://www.apache.org/licenses/icla.pdf) and [Apache Corporate Contributor License Agreement v2.0](https://www.apache.org/licenses/cla-corporate.pdf), with the modifications described in §6 below.

Two forms apply:

- **Individual CLA (ICLA)**: for contributions made by an individual in a personal capacity.
- **Corporate CLA (CCLA)**: for contributions made by an employee on behalf of an employer, or by an employer authorising designated employees to contribute on its behalf.

If you are unsure which applies, default to the ICLA. The project owner will follow up if a CCLA is needed.

---

## 1. Definitions

- **"Project"** means Verzeta Studio, as distributed by the project owner from the canonical repository.
- **"Project Owner"** means the natural or legal person who maintains the canonical repository and to whom this CLA assigns rights.
- **"You"** (or **"Your"**) means the individual signing this CLA in the case of the ICLA, or the corporate entity signing in the case of the CCLA.
- **"Contribution"** means any original work of authorship, including any modifications or additions to existing work, that You submit to the Project in any form. "Submit" includes any form of electronic, verbal, or written communication sent to the Project Owner or the Project's communication channels (pull requests, patches, issue attachments, mailing-list posts) except for those marked or otherwise designated in writing as "Not a Contribution."

---

## 2. Grant of Copyright Assignment

You hereby **assign** to the Project Owner all right, title, and interest worldwide in and to all Contributions, including all associated intellectual property rights: copyright, neighbouring rights, sui generis database rights, moral rights to the extent assignable, and all renewals and extensions.

This is an **assignment**, not a license. After the assignment, the Project Owner is the copyright holder of Your Contribution and may exercise all rights of a copyright holder, including:

- Releasing the Contribution under GPL-3.0-or-later, LGPL-3.0-or-later, or any other license including future commercial licenses.
- Sublicensing the Contribution to commercial integrators on negotiated terms.
- Combining the Contribution with other code under any license.
- Modifying or removing the Contribution at the Project Owner's discretion.

The assignment is irrevocable. After You submit a Contribution and the Project Owner accepts it into the canonical repository, You cannot withdraw the assignment.

> **Why assignment instead of a license grant?** The Project operates a commercial-license track alongside the open-source GPL / LGPL tracks (see [LICENSING.md](LICENSING.md)). A license grant lets the Project Owner _use_ Your Contribution; an assignment lets the Project Owner _relicense_ it. The commercial track requires the right to relicense. Without assignment, every commercial-license deal would need a separate permission from every contributor whose code touched the deal's scope. That blocker is what assignment removes.

You retain the right to use Your Contribution in Your own separate work under any license You choose; the assignment is non-exclusive in that direction. You also retain credit for authorship (see §7).

---

## 3. Grant of Patent License

You hereby grant to the Project Owner and to recipients of the Project (in any of its license tracks) a perpetual, worldwide, non-exclusive, no-charge, royalty-free, irrevocable patent license to make, have made, use, offer to sell, sell, import, and otherwise transfer the Contribution and any combinations of the Contribution with other code in the Project. The patent license applies only to those patent claims licensable by You that are necessarily infringed by Your Contribution alone or by combination of Your Contribution with the Project.

**Patent retaliation.** If any entity institutes patent litigation against You or any other entity (including a cross-claim or counterclaim in a lawsuit) alleging that the Contribution, or the Project itself, infringes a patent owned or licensable by that entity, then any patent licenses granted to that entity under this CLA are terminated as of the date such litigation is filed.

---

## 4. Warranties

You represent and warrant that:

a. The Contribution is **Your original work**. To the extent it incorporates other materials, You have the right to assign those materials under this CLA and have called out their source in the Contribution itself.

b. Your Contribution does not knowingly violate any third party's patents, copyrights, trade secrets, or other intellectual property rights, and is free of any third-party encumbrances (employment agreements, prior contracts, NDAs) that would prevent Your assignment.

c. If You are signing the CCLA on behalf of a corporate entity, You are authorised to do so and the listed designated contributors are authorised by that entity to submit Contributions under the assignment terms.

d. The Contribution contains no malicious code, no deliberate vulnerabilities, no telemetry or "phone home" mechanisms beyond what is documented in the Project's [PRIVACY.md](PRIVACY.md), and no backdoors of any kind.

You are not expected to provide support for Your Contribution. You may, but You are not obligated to.

---

## 5. Disclaimers

The Contribution is provided **AS IS**, without warranty of any kind, express or implied. The Project Owner does not undertake to incorporate Your Contribution; the Project Owner may accept, reject, modify, or remove Contributions at any time and for any reason. No formal agreement or partnership is created by this CLA.

---

## 6. Modifications from Apache CLA v2.0

This CLA differs from the canonical Apache ICLA / CCLA v2.0 in the following ways. All other terms (definitions, patent grant, retaliation, warranties, disclaimers) track Apache's wording.

| Section            | Apache                                                                                                                            | Verzeta                                                                                                                                    | Reason                                                                                                            |
| ------------------ | --------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------ | ----------------------------------------------------------------------------------------------------------------- |
| §2 (copyright)     | License grant ("You hereby grant…a perpetual, worldwide, non-exclusive, no-charge, royalty-free, irrevocable copyright license…") | **Assignment** ("You hereby assign to the Project Owner all right, title, and interest…")                                                  | Required for the commercial-license track per [LICENSING.md](LICENSING.md).                                       |
| §4 (warranties)    | Apache CLA §5: "You represent that each of Your Contributions is Your original creation"                                          | Strengthened to include the malicious-code / no-backdoors / no-undocumented-telemetry warranty (clause **d**).                             | Reflects the Project's [SECURITY.md](SECURITY.md) "no backdoor introductions" commitment back at the contributor. |
| Sign-off mechanism | Out-of-band PDF signature                                                                                                         | CLAassistant bot integrated with the project's hosting platform (one-time accept on first PR; thereafter automatic per-PR sign-off check). | Reduces friction.                                                                                                 |

These are the _only_ material modifications. Where this CLA is silent on a topic and the Apache CLA is not, the Apache wording is the fall-back source of truth.

---

## 7. Attribution and credit

You retain the right to be credited as the author of Your Contribution. Contributors are credited through:

- The Git commit history (commits land with You as the `Author:` and the Project Owner as the `Committer:` after squash-merge; your authorship is permanent).
- The project's release notes, where the Project Owner chooses to mention a Contribution.

If You request anonymity (for example, You are an employee whose employer has not consented to disclosure), the Project Owner accommodates that by using a generic author identity. The CLA itself is non-public; only Your acceptance event is recorded by the bot.

---

## 8. How to sign

When You open Your first pull request, the CLAassistant bot comments with a link inviting You to accept this CLA. You read this document, click "I agree", and the bot records Your acceptance against Your repository-hosting identity. Subsequent pull requests authored by the same identity pass through automatically.

Corporate contributors: if You are submitting on behalf of a corporate entity, contact the Project Owner via the email in [SECURITY.md](SECURITY.md) before opening Your first PR. The Project Owner will guide You through the CCLA acceptance, including naming the designated contributors who are authorised to submit on the entity's behalf.

---

## 9. Termination

The assignment in §2 is irrevocable. The patent license in §3 terminates only as described in §3 (patent retaliation). The warranties in §4 survive termination of any other rights.

The Project Owner may, at the Project Owner's sole discretion, remove Your Contribution from the canonical repository at any time. Removal does not unwind the assignment of any version that was distributed before removal.

---

## 10. Choice of law and forum

This CLA is governed by the laws of the jurisdiction in which the Project Owner is registered, without reference to its conflict-of-laws principles. Any dispute arising out of or relating to this CLA is subject to the exclusive jurisdiction of the courts of that jurisdiction.

The jurisdiction is recorded in the public-facing [GOVERNANCE.md](GOVERNANCE.md).

---

## 11. Entire agreement

This CLA constitutes the entire agreement between You and the Project Owner with respect to contributions to the Project. It supersedes any prior agreement or understanding on the same subject.

The Project Owner may revise this CLA from time to time. Material changes apply only to Contributions submitted after the revision takes effect; previously-submitted Contributions remain governed by the version of the CLA in effect at the time of their acceptance.

The current version of this CLA is recorded in this file's Git history; the version in effect when You signed is the one whose Git commit hash is recorded against Your acceptance event by the CLAassistant bot.

---

## 12. Contact

Questions about the CLA itself, the assignment terms, or the corporate-contributor process: write to the address in [SECURITY.md](SECURITY.md) with the subject prefix `[Verzeta CLA]`.

---

Last reviewed: 2026-05.
