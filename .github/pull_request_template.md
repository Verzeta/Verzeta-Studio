<!--
SPDX-FileCopyrightText: 2026 Aditya Mehra <aix.m@outlook.com>
SPDX-License-Identifier: LGPL-3.0-or-later

Do NOT delete any section or checklist item of this template. A PR that removes
the template or leaves the checklist blank does not meet the contribution
requirements and will be flagged automatically. If an item genuinely does not
apply, tick it and add "(N/A: reason)".
-->

## Summary

<!-- One or two sentences: what does this change do? -->

## Why

<!-- The reasoning. What problem does it solve? Reference design discussion if any. -->

## Linked issue

<!-- Required. Bug fixes and features must reference an issue. -->
Closes #

## Type of change

- [ ] Bug fix (non-breaking change that fixes a defect)
- [ ] New feature (non-breaking change that adds behaviour)
- [ ] Breaking change (behaviour changes for existing users)
- [ ] Documentation only

## Build and test proof

<!--
REQUIRED. Paste the real output of a full build and the test suite from your
machine. "It builds on my end" is not proof: paste the last lines showing the
build completed and `100% tests passed`. See BUILDING.md and CONTRIBUTING.md.
-->

```text
$ cmake --preset debug && cmake --build build-debug --parallel
# ... paste the last lines showing a clean build ...

$ ctest --test-dir build-debug --output-on-failure
# ... paste the summary line: "100% tests passed, 0 tests failed out of N" ...
```

## Contributor checklist

<!-- Every box must be ticked. See CONTRIBUTING.md. -->

- [ ] The project **builds cleanly** and I pasted the proof above.
- [ ] `ctest` passes at **100%** locally (proof above).
- [ ] **New feature → new tests**, or **bug fix → a regression test that fails without the fix**.
- [ ] Code is formatted (`clang-format` for C++, `qmlformat` for QML) and passes `clang-tidy`.
- [ ] Every new file carries the correct **SPDX headers**; `reuse lint` passes.
- [ ] Public C++ API has **Doxygen** headers (`@brief`, `@param`, `@returns`); no internal/plan context leaked into published comments.
- [ ] If user-visible behaviour changed, I updated the relevant page under `Documentation/User/`.
- [ ] Commit messages follow the project style (single-line summary, then a body explaining **why**).
- [ ] I have signed off on the [CLA](https://github.com/Verzeta/Verzeta-Studio/blob/main/CLA.md) (the CLA bot will guide first-time contributors).
- [ ] This PR does **one thing** and is **not** a declined category (telemetry, phone-home, forced accounts, SPDX removal/relicensing, or a style-only mass change). See CONTRIBUTING.md.

## Notes for the reviewer

<!-- Anything that helps review: trade-offs, follow-ups, screenshots for UI changes. -->
