# Incident Response Plan

This document defines the security-specific response process for Ladybug. It complements the [Security Policy](../../SECURITY.md).

---

## Scope

This plan covers security incidents affecting any of the following surfaces:

| Surface | Location |
|---------|----------|
| Core releases | https://github.com/LadybugDB/ladybug/releases |
| CLI binary | https://github.com/LadybugDB/ladybug/releases |
| Python binding | PyPI — `ladybug` package (`tools/python_api` submodule) |
| Node.js binding | npm — `@ladybugdb/core` package (`tools/nodejs_api` submodule) |
| Java binding | Maven Central — `com.ladybugdb:ladybug` (`tools/java_api` submodule) |
| Rust binding | crates.io — `lbug` crate (`tools/rust_api` submodule) |
| WASM binding | npm — `@ladybugdb/wasm-core` package (`tools/wasm` submodule) |
| Go binding | https://github.com/LadybugDB/go-ladybug |
| Swift binding | https://github.com/LadybugDB/swift-ladybug |
| Explorer | https://github.com/LadybugDB/explorer |
| bugscope | https://github.com/LadybugDB/bugscope |
| Documentation site | https://docs.ladybugdb.com |
| Main website | https://ladybugdb.com |

---

## Severity Levels

| Level | Criteria | Target response SLA |
|-------|----------|---------------------|
| **P0 — Critical** | RCE, auth bypass, data exfiltration, supply-chain compromise of a published artifact, or defacement of a public-facing site | Acknowledge in **2 hours**; patch/mitigation in **24 hours** |
| **P1 — High** | Data corruption, privilege escalation, significant info-disclosure, or compromised signing key | Acknowledge in **12 hours**; patch/mitigation in **72 hours** |
| **P2 — Medium** | Limited info-disclosure, DoS against a single user, or incorrect security-sensitive documentation | Acknowledge in **3 business days**; patch/mitigation in **14 days** |
| **P3 — Low** | Defense-in-depth improvements, hardening suggestions | Acknowledge in **7 business days**; handled in normal sprint cycle |

---

## Roles

| Role | Responsibility |
|------|---------------|
| **Incident Commander (IC)** | Owns the response end-to-end; makes go/no-go decisions; coordinates communication. |
| **Security Lead** | Triages and confirms severity; drives technical investigation and patch. |
| **Maintainer** | Owns Core, Language Bindings, and CLI; responsible for patched releases on their registry. |
| **Infra/Site Owner** | Owns docs and website rollback, CDN purge, and hosting-provider contacts. |
| **Comms Lead** | Drafts and publishes user-facing advisories, coordinates disclosure timeline with reporter. |

For incidents reported externally, the Security Lead is the first responder and assigns an IC within 2 hours of receipt.

---

## Phase 1 — Detection and Triage

### 1.1 Intake channels

- **External reports:** [security@ladybugdb.com](mailto:security@ladybugdb.com) (primary) or GitHub private vulnerability reporting on the core repo.
- **Internal discovery:** Any team member who finds a potential security issue opens a **private** GitHub Security Advisory draft — never a public issue.
- **Automated signals:** CodeQL (`codeql.yml`), Dependabot alerts, CI pipeline anomalies.

### 1.2 Triage checklist

Within the SLA above, the Security Lead must:

- [ ] Reproduce or confirm the report in a private environment.
- [ ] Identify affected versions (check [Supported Versions](../../SECURITY.md)) and affected surfaces (engine, bindings, docs, site).
- [ ] Assign a severity level (P0–P3).
- [ ] Assign an Incident Commander.
- [ ] Open a private tracking channel (e.g. a private Slack channel or GitHub Security Advisory) — **never discuss details in public channels**.
- [ ] Acknowledge the reporter with the assigned severity and expected timeline.

---

## Phase 2 — Containment

Actions depend on which surface is affected.

### 2.1 GitHub releases — compromised or vulnerable artifact

1. **Do not delete** the release immediately; coordinate with GitHub Support if the release page itself is compromised.
2. Yank/retract the artifact from all downstream registries (see §2.2–2.4).
3. If a signing key or CI secret is suspected to be leaked:
   - Rotate the secret immediately in GitHub repository settings.
   - Invalidate all existing signed artifacts until a clean signing key is used.
4. Add a banner to the affected release on GitHub: _"This release contains a known security issue. Upgrade to vX.Y.Z."_

### 2.2 Language binding registries

For each affected binding, the Maintainer must:

| Registry | Yank / retract command |
|----------|------------------------|
| PyPI | `pip` package maintainer → use [PyPI project management](https://pypi.org/manage/project/ladybug/releases/) to yank the release |
| npm (Node.js / WASM) | `npm deprecate <pkg>@<version> "Security vulnerability — upgrade to <fixed>"` |
| Maven Central (Java) | Contact Sonatype support; mark artifact as insecure in the metadata |
| crates.io (Rust) | `cargo yank --version <version> --crate <crate>` |
| Go / Swift | Revert to the previous release |

After yanking, publish a patched version as quickly as possible (see Phase 3).

### 2.3 Documentation site — docs.ladybugdb.com

- If content is defaced or maliciously modified, roll back to the last known-good deployment.
- Contact the hosting provider to take the affected page offline if rollback is not instant.
- Purge the CDN cache after rollback.
- If inaccurate security guidance is published (e.g. wrong mitigation steps), add a visible warning banner immediately while the correction is prepared.

### 2.4 Main website — ladybugdb.com

- If defaced, redirect all traffic to a static maintenance page via CDN rule while the site is restored.
- Contact the hosting provider and domain registrar if DNS or TLS is suspected to be compromised.
- Restore from the last verified clean deployment.
- Purge CDN cache after restore.

---

## Phase 3 — Eradication and Patching

### 3.1 Core engine fix

1. Develop the fix on a **private fork or branch** — do not push to a public branch until the advisory is ready to publish.
2. Write a targeted regression test that fails before the fix and passes after.
3. Build and test all affected platforms via the relevant CI workflows (`ci-workflow.yml`, `multiplatform-build-test.yml`, binding workflows, etc).
4. Have at least one other engineer review the fix before merge.

### 3.2 Coordinated release

Prepare all patched artifacts in parallel and publish them to their respective registries within the same release window (ideally within 30 minutes of each other) to avoid a gap where users are directed to upgrade but no fixed version is available.

### 3.3 Verify artifact integrity

After publishing, verify that:

- [ ] The correct version is live on every registry.
- [ ] SHAs and checksums on the GitHub release page match the artifacts.
- [ ] Any code-signing signatures are valid.
- [ ] Docs and the website reference the new version.

---

## Phase 4 — Disclosure

Ladybug follows a **90-day coordinated disclosure** timeline from the date a vulnerability is confirmed (see [SECURITY.md](../../SECURITY.md)).

### 4.1 GitHub Security Advisory

- Publish the advisory the same day (or within hours) as the patched release.
- Include: CVE ID (request one from MITRE if not yet assigned), affected versions, fixed version, severity (CVSS score), and a concise description.
- Credit the reporter by name/handle unless they request anonymity.

### 4.2 User-facing announcement

Publish to all relevant channels simultaneously with the advisory:

- GitHub release notes for the patch version.
- A pinned post or announcement in the project's community forum / Discord / mailing list.
- Update the documentation site to reflect the fixed version.

Announcement must include:

- What the vulnerability is (enough detail to assess exposure, not a full exploit guide before patching is widespread).
- Which versions are affected and which version is safe.
- Upgrade instructions for each affected language binding.
- Workarounds, if any, for users who cannot upgrade immediately.

---

## Phase 5 — Recovery and Validation

- [ ] Monitor download counts and support channels for reports of the unfixed version still being used.
- [ ] Confirm CDN and mirrors have propagated the yanked/updated artifacts.
- [ ] Restore documentation site and website to fully operational state.
- [ ] Verify no residual attacker access (rotate any potentially exposed credentials).

---

## Phase 6 — Post-Incident Review

After the incident is fully resolved, write a document explaining the incident, its impact, root cause, resolution, and lessons learned

### Where to store documents

- **Location:** `security/incidents/`
- **Naming:** `YYYY-MM-DD-security-<slug>.md`
- **Format:** Markdown stored in the repo so the document is versioned and traceable from the fix commit.
- **Index:** Add a row to the index table in this plan with a short description and link to the document.

### Recommended structure

Use these sections so reports are consistent and easy to scan:

| Section | Purpose |
|--------|--------|
| **Author** | Optional. Your name or handle for attribution and traceability. |
| **CVE / advisory reference:** | Link to the published GitHub Security Advisory. |
| **Reporter credit** | Unless anonymity was requested. |
| **Summary** | 2-4 sentences: what failed, where, and the one-line root cause. |
| **Impact** | Who or what was affected (tests, users, CI) and severity. |
| **Root cause** | Why it happened: code, API, contract, or operational mismatch. |
| **Timeline** | Short chronology: when the failure was seen, when it was diagnosed, when the fix was applied. |
| **Disclosure timeline:** | Dates of report → triage → patch → advisory → public disclosure. |
| **Resolution** | What changed and how to verify it. |
| **Registry / artifact impact:** | Which binding versions were yanked, when, and restored. |
| **Lessons learned** | What to do differently next time. |
| **Action items** | Optional follow-ups with owner and status if applicable. |
| **References** | Links to files, tests, advisories, and commits that matter. |

Keep reports blameless: focus on systems, APIs, automation, and process rather than individuals.

### Industry practices we follow

- **Blameless post-mortems:** describe what happened and why the system allowed it, not who caused it.
- **Single, versioned location:** all incident docs live under `security/incidents/` so they are searchable and tied to the codebase.
- **Dated, descriptive filenames:** filenames sort chronologically and stay easy to scan.
- **Structured sections:** every report uses the same core headings.
- **Traceability:** documents should reference source files, tests, advisories, and commits when useful.

---

## Quick-Reference Checklist (P0/P1)

```
[ ] Confirm and reproduce the issue privately
[ ] Assign severity and Incident Commander
[ ] Open private tracking channel
[ ] Acknowledge reporter
[ ] Contain: yank/retract affected artifacts or take site offline
[ ] Rotate any leaked secrets
[ ] Develop fix on private branch
[ ] Write regression test
[ ] Peer review the fix
[ ] Coordinate patched release across all affected surfaces
[ ] Verify artifact integrity on all registries
[ ] Publish GitHub Security Advisory + CVE
[ ] Publish user-facing announcement on all channels
[ ] Monitor adoption of fix
[ ] Write post-incident document in security/incidents/
[ ] Update the incident index in this plan
```

---

## Index of incidents

| Date | Document | Short description |
|------|----------|-------------------|

*(Add new rows here when you add a new incident document.)*
