# Ladybug Threat Model

This document describes the current threat model for Ladybug as an embedded, serverless graph database. It is meant to guide secure design, code review, and release decisions for the core engine, official extensions, language bindings, CLI, and Wasm distribution.

## Scope

In scope:

- The core database engine and query execution runtime
- On-disk database files, WAL, checkpoints, and recovery paths
- Language bindings and shell entry points that expose Ladybug in-process
- Official and user-provided extensions, including install/load flows
- File, connector, and remote data access surfaces exposed by core or extensions
- The Wasm/browser and Node.js Wasm distributions

Out of scope:

- Security of the embedding application beyond Ladybug's API boundary
- Host OS, filesystem, container, and network hardening
- Authentication, authorization, tenancy, and perimeter controls implemented outside Ladybug
- Security properties of third-party services that Ladybug connects to

## System overview

Ladybug is an embeddable database that executes in the same process as the host application. It accepts Cypher queries and data-import commands, reads and writes local database files, exposes multiple language bindings, and can be extended with native extensions. Some official extensions add filesystem, cloud, and remote-database access. The Wasm distribution runs the database in a browser or Node.js worker context.

That deployment model gives Ladybug a small default network surface, but it also means memory-safety bugs, unsafe extension loading, or abusive queries can directly impact the host process.

## Security objectives

Ladybug should:

1. Preserve database integrity across normal execution, crashes, rollback, and recovery.
2. Avoid memory corruption, arbitrary code execution, and unsafe native loading from untrusted inputs.
3. Prevent unintended filesystem or network access beyond what the embedding application explicitly enables.
4. Protect confidential data handled by queries, database files, credentials, and connector configuration.
5. Degrade safely under malformed input or resource exhaustion attempts.

## Assets

The main assets protected by this threat model are:

- Graph data stored in tables, indexes, WAL, and checkpoints
- Query results returned to the embedding application
- Database metadata, schema, statistics, and extension state
- Host filesystem paths reachable through open, attach, copy, export, or extension features
- Secrets used by extensions and connectors
- Availability of the embedding process and user workloads
- Integrity of extension artifacts and release packages

## Assumptions and trust boundaries

### Assumptions

- Ladybug usually runs with the same privileges as the calling application.
- Anyone who can submit queries or choose import/export paths may influence local resource usage.
- Official extensions are more trusted than arbitrary user-provided extensions, but both run native code in-process once loaded.
- Wasm builds inherit the browser or Node.js sandbox they run inside; they do not create a stronger sandbox on their own.

### Trust boundaries

1. **Host application -> Ladybug API**  
   Query text, parameters, data files, configuration, and paths cross from the embedding application into the engine.
2. **Ladybug -> local filesystem**  
   Database files, attached databases, COPY/LOAD/EXPORT sources, extension directories, and WAL/checkpoint files cross into OS-managed storage.
3. **Ladybug -> remote services**  
   Extensions such as `httpfs`, `azure`, `postgres`, `sqlite`, `duckdb`, `delta`, `iceberg`, `neo4j`, `unity_catalog`, and `llm` can expand the trust boundary to remote endpoints and external credentials.
4. **Ladybug -> native extension code**  
   `INSTALL` and `LOAD EXTENSION` introduce dynamically loaded native code into the database process.
5. **Wasm package -> browser/Node.js runtime**  
   Worker scripts, IndexedDB/IDBFS or Node.js filesystem access, and web application code form a separate threat surface from the native engine.

## Main attack surfaces

- Cypher parsing, planning, optimization, and execution
- `COPY`, `LOAD FROM`, `ATTACH`, `EXPORT`, and other file-backed operations
- Database open/recovery/checkpoint/WAL replay on attacker-controlled files
- Dynamic extension installation and loading
- Remote connector and object-store integrations
- CLI and language bindings that pass untrusted input into native code
- Wasm worker loading, persistence, and browser-exposed APIs

## Threats, current mitigations, and residual risk

| Threat | Why it matters | Current posture | Residual risk |
| --- | --- | --- | --- |
| Memory corruption from malformed queries, data files, database files, or extension inputs | Ladybug runs in-process, so parser, execution, storage, or recovery bugs can crash or compromise the host application | Keep unsafe parsing and recovery paths heavily tested; use sanitizers in development builds; treat malformed files and query text as hostile inputs | **High** because the core is native C++ and parses complex formats |
| Denial of service via expensive queries or large imports | Untrusted workloads can exhaust CPU, memory, disk, threads, or lock write progress | Transactions, WAL, and recovery are designed for correctness; host applications should set workload limits and isolate untrusted workloads | **High** for multi-tenant or user-facing embeddings without external limits |
| Malicious or corrupted database/WAL/checkpoint files | Opening attacker-controlled database directories can trigger parser/recovery bugs or corrupt state | Recovery, rollback, and checkpoint correctness are part of the trusted computing base and should reject invalid state explicitly | **High** when opening files from untrusted origins |
| Filesystem abuse through path-based features | `COPY`, `LOAD FROM`, `ATTACH`, `EXPORT`, and extensions can read or write attacker-selected paths | Path access is an intended feature, not a sandbox; deployments must rely on OS/container permissions and careful API exposure | **High** if the host exposes these features to untrusted users |
| Remote code execution through extensions | `LOAD EXTENSION` loads native libraries with process privileges, and `INSTALL` downloads extension artifacts before loading them | Only load trusted extensions from trusted repos; official extensions reduce but do not eliminate risk | **Critical** because extension code executes natively in-process |
| Supply-chain compromise of extension or release artifacts | A compromised artifact can execute code during install/load or leak data | Secure release provenance, artifact signing/checksumming, and least-privilege extension repos are required operational controls | **High** until strong provenance verification is enforced everywhere |
| SSRF or data exfiltration through network-enabled extensions | Connectors and cloud/object-store extensions can reach internal or external services using host credentials | Treat network-enabled extensions as privileged features; disable or omit them where unnecessary | **High** in environments with sensitive internal network reachability |
| Credential leakage through config, logs, errors, or persistence | Extension options or connector settings may contain secrets | Mark secret-bearing options confidential, avoid logging secrets, and document secret-handling expectations | **Medium** if confidential handling stays consistent across bindings and tooling |
| Integrity failures around transactions, rollback, checkpoint, or recovery | Bugs here can lose data, expose stale data, or violate ACID guarantees | Transaction and recovery logic are core security-relevant components and should be regression-tested like security code | **Medium** for trusted workloads, **High** for hostile crash/recovery inputs |
| Unsafe assumptions about auth or tenant isolation | Ladybug is embedded and does not provide a network security boundary by itself | Document that authentication, authorization, auditing, and tenant isolation belong to the embedding application or platform | **High** if users deploy it as though it were a self-contained secure server |
| Browser/Wasm misuse | Browser apps may expose the DB to hostile page code, persistent IDBFS data, or untrusted worker paths | Wasm reduces native-host escape risk relative to a native embedding, but app-origin security still governs confidentiality and integrity | **Medium** in well-isolated apps, **High** in hostile or XSS-prone web apps |

## Threat model decisions

The current project-level position is:

- **Untrusted query text is supported**, but it must not be treated as trusted input by the engine.
- **Untrusted database files, WAL, and imported files are risky** and should be treated as hostile inputs.
- **Native extensions are trusted code once loaded.** Loading an extension is a privilege boundary crossing, not a routine low-risk operation.
- **Filesystem and network access are capabilities, not bugs.** If exposed to untrusted users, they require controls outside Ladybug.
- **Ladybug is not a security perimeter.** It should be deployed behind application, OS, container, and network controls appropriate to the environment.

## Highest-risk areas to revisit

This threat model should be updated whenever Ladybug changes any of the following:

- Extension install or load trust policy
- Remote connector or object-store support
- Database file format, WAL, recovery, or checkpoint behavior
- Wasm packaging, worker bootstrap, or browser persistence model
- Any feature that broadens filesystem access, remote access, or code-loading behavior
