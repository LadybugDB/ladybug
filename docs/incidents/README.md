# Incident and post-mortem documentation

This directory holds **incident reports** and **post-mortem documents** for significant bugs, test failures that reveal design or contract issues, and production or CI incidents. The goal is to learn from failures and make the reasoning behind fixes traceable.

---

## When to write an incident document

Write a document when one or more of the following apply:

- An E2E or important test fails and the root cause is non-obvious or involves API/contract misuse.
- A bug affects correctness or stability and the fix is worth explaining for future maintainers.
- A production or CI outage or regression occurs and you want a blameless record of what happened and how it was fixed.
- The same class of bug could recur (e.g. inconsistent use of APIs) and you want to capture the lesson.

You do **not** need a full document for trivial one-line fixes or typos unless they had notable impact.

---

## Where to store documents

- **Location:** `docs/incidents/`
- **Naming:** `YYYY-MM-DD-short-slug.md` (e.g. `2026-02-16-copy-rel-segment-planner-schema-groups.md`).
- **Format:** Markdown; store in the repo so the document is versioned and traceable from the fix commit.

---

## Recommended structure of an incident document

Use these sections so reports are consistent and easy to scan:

| Section | Purpose |
|--------|--------|
| **Author** | Optional. Your name or handle for attribution and traceability. |
| **Summary** | 2–4 sentences: what failed, where, and the one-line root cause. |
| **Impact** | Who/what was affected (tests, users, CI) and severity. |
| **Root cause** | Why it happened: code/API/contract involved and the exact mismatch or bug. |
| **Timeline** | Short chronology: when the failure was seen, when it was diagnosed, when the fix was applied. |
| **Resolution** | What was changed (file(s), condition, code) and how to verify (command or test). |
| **Lessons learned** | What to do differently (e.g. use one consistent API, align similar code paths). |
| **Action items** | Optional follow-ups (tests, comments, docs) with owner/status if applicable. |
| **References** | Links to files, tests, and commits that matter. |

Keep the report **blameless**: focus on systems, APIs, and process, not on individuals.

---

## Industry practices we follow

- **Blameless post-mortem** (e.g. Google SRE): describe what happened and why the system allowed it, not who “caused” it.
- **Single, versioned location**: all incident docs live under `docs/incidents/` in the repo so they are searchable and tied to the codebase.
- **Dated, descriptive filenames**: `YYYY-MM-DD-slug.md` makes ordering and discovery easy.
- **Structured sections**: Summary, Impact, Root cause, Resolution, Lessons learned (and optional Timeline, Action items) so every report can be read the same way.
- **Traceability**: documents reference source files, tests, and (where useful) commits so future readers can jump from doc to code.

---

## Index of incidents

| Date | Document | Short description |
|------|----------|-------------------|
| 2026-02-16 | [2026-02-16-copy-rel-segment-planner-schema-groups.md](2026-02-16-copy-rel-segment-planner-schema-groups.md) | CopyRelSegmentTest failure; wrong schema group check in `planCopyRelFrom` vs Partitioner’s single-group requirement. |

| 2026-02-16 | [2026-02-16-connection-close-sigsegv.md](2026-02-16-connection-close-sigsegv.md) | SIGSEGV when Connection destroyed while query workers still running; fix: wait for in-flight queries before destroying ClientContext. |

| 2026-02-17 | [2026-02-17-minimal-test-checkpoint-timeout.md](2026-02-17-minimal-test-checkpoint-timeout.md) | CI minimal test: CloseConnectionWithActiveTransaction fails in TearDown; checkpoint times out because Connection destructor skips rollback and transaction stays in manager. |

| 2026-02-17 | [2026-02-17-fsm-leak-copy-rollback-recovery.md](2026-02-17-fsm-leak-copy-rollback-recovery.md) | FSM leak after COPY + ROLLBACK + RELOAD: two e2e tests see 95 used pages instead of 4; analysis and options (audit allocation paths, checkpoint/reload). |

*(Add new rows here when you add a new incident document.)*
