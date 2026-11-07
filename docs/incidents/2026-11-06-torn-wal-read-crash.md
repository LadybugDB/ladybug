# Incident: opening a database crashes on a WAL torn past a read-buffer boundary

**Date:** 2026-11-06
**Status:** Resolved in 0.22.3 and on `main`
**Severity:** Low as a security issue, since an attacker needs access to the WAL. A crashing open is a serious reliability failure.
**Component:** Storage: `BufferedFileReader`, WAL recovery
**Author:** [@zahariash](https://github.com/zahariash)

---

## Summary

Opening a database could segfault when its WAL ended past a 4 KB read-buffer boundary, inside a record that crossed it. This happened with `throw_on_wal_replay_failure` on or off, and each new open crashed the same way. `BufferedFileReader::read` copied the rest of a read that crossed the buffer without checking how much the next page held. A plain process kill during a multi-page commit could leave such a WAL.

## Impact

- **Reliability:** a process killed during a commit, or a write stopped by a file-size limit, could leave a database that crashes the process on every open.
- **Security:** bytes from the WAL reached a heap pointer: the crash was `free(0x2d302d302d302d30)`, the ASCII text `0-0-0-0-` of a string the WAL held. `security/threat_model.md` lists WAL replay on attacker-controlled files as an attack surface. The maintainer sees no way to remote code execution through remote Cypher. Exploitability, and whether queries alone can reach the crash, were not assessed.
- **Affected:** 0.15.3, 0.16.0, 0.21.2, 0.22.1 and `main` at `24b5a15e0` were tested and crash. 0.15.3 is the oldest wheel tested. Not every older release will be patched.
- **Scope:** only some torn lengths crash. On `main` at `24b5a15e0`, in a two-commit WAL with 974-byte strings, 625 of 4,307 cut positions in the second commit crash with checksums off, and 713 of 4,355 with them on, 64 of those as aborts. With 6000-byte strings, 14 positions crash in each mode.

## Root cause

`BufferedFileReader::read` served a read that crossed its buffer in two parts: the bytes left in the buffer, then the remainder from the next page, copied after `readNextPage()`. It did not compare the remainder with the new `bufferSize`. At the end of a torn file that page is short, so the copy took stale bytes from the previous page and `bufferOffset` passed `bufferSize`, after which `bufferSize - bufferOffset` can underflow. This comes from reading the code, and the exact path from there to the crash was not traced. The observed facts are a crash in `free` during `WALRecord::deserialize` under `WALReplayer::dryReplay` on the 4233-byte WAL of a minimal script that stops at a file-size limit, and that changing this function removes every crash in the sweeps.

No short write is needed to reach it. WAL writes continue from the end of the last commit, so they are not aligned to the read buffer. On `main` with checksums on, after a first commit of 497 bytes, a 4096-byte write ends at 4593, inside the crashing range. Killing the process before its next write leaves this file.

The WAL tail tests probably missed it because they cut files at a few fixed positions. The simulator found it through a rule that writes long strings up to a file-size limit and then kills the process, here strings of about 1 KB and a 4233-byte limit.

## Timeline

All on 2026-11-06.

- The workload simulator, running `main` with open recovery PRs, hit a crash on reopen. A byte sweep and a minimal script followed, and the crash reproduced on released wheels.
- A code review of the report's draft challenged its claim that a short write was required. A test that kills the process at each write showed that a plain kill is enough.
- The report went to security@ladybugdb.com, following `SECURITY.md`. The maintainer classified it as low severity and planned 0.22.3.
- 17:22 UTC: [#1520](https://github.com/LadybugDB/ladybug/pull/1520) merged to `main` (`1e71d9348`).
- 17:45 UTC: [#1522](https://github.com/LadybugDB/ladybug/pull/1522) merged to `release_0_22` (`ec35ca5145`).
- 20:40 UTC: 0.22.3 published on PyPI.

## Resolution

`BufferedFileReader::read` now loops over pages for small reads, so `readNextPage()` throws at the end of the file, and checks large reads against the file size before reading them. A tail torn inside a record now surfaces as a replay error: a strict open raises (with checksums on, the minimal script gets `Checksum verification failed, the WAL file is corrupted.`), and a lenient open drops the torn transaction and keeps the earlier commits. A cut on a record boundary opens with the earlier commits in both modes. The same reader also serves the database header, the shadow file and the checkpoint, and none of those files was cut in the checks below.

**Verify:** `WalTest.TornWALTailInsideLargeStringRecord`.

On the head of [#1520](https://github.com/LadybugDB/ladybug/pull/1520), four two-commit WALs (checksums on and off, 974- and 6000-byte strings) were cut at every byte of the second commit, 33,140 cuts in total, and opened strict and lenient. None crashed or returned an unexpected row count. On 0.22.3, the minimal script and the kill-at-each-write test with checksums on no longer crash.

## Lessons learned

- A refill can return fewer bytes than a read still needs. A reader of on-disk data must loop and check, and treat file contents as hostile, as the threat model says.
- Torn-write tests need to sweep cut positions and include long values. A few fixed cuts miss a failure that exists only in part of one record.
- Reachability should be tested rather than reasoned about. The first analysis assumed that a kill lands on a page boundary, which understated the bug.
- A recovery that succeeds truncates the torn tail, so a wrong result found later has lost its evidence. Keeping a copy of the files from before recovery makes such failures repeatable.

## Action items

| Item | Status |
|------|--------|
| Fix and regression test | Done: [#1520](https://github.com/LadybugDB/ladybug/pull/1520), [#1522](https://github.com/LadybugDB/ladybug/pull/1522), 0.22.3 |
| Regression test with a value over 4 KB | Open: the current test uses 3000-byte values |
| Simulator rule that cuts a WAL at chosen points and predicts which commits survive | Written, not yet published |
| `wal_dump` reading a WAL by name and reporting where it stopped, with a test | Open: [#1526](https://github.com/LadybugDB/ladybug/pull/1526) |
| What a lenient open keeps after mid-file damage | Open: [#1497](https://github.com/LadybugDB/ladybug/issues/1497) |

## References

- `src/common/serializer/buffered_file.cpp`, `BufferedFileReader::read`
- `test/transaction/wal_test.cpp`, `TornWALTailInsideLargeStringRecord`
- `security/threat_model.md`, `SECURITY.md`
- `1e71d9348` on `main`, `ec35ca5145` on `release_0_22`
- [#1520](https://github.com/LadybugDB/ladybug/pull/1520), [#1522](https://github.com/LadybugDB/ladybug/pull/1522), [#1497](https://github.com/LadybugDB/ladybug/issues/1497), [#1526](https://github.com/LadybugDB/ladybug/pull/1526)
