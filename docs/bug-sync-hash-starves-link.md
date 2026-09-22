# Bug: sync fails on large trees, and a dry run reports the failure as success

Status: fixed 2026-09-22. Found 2026-09-22 against agent `0.1.0_20260922.1801.win98`,
client `0.1.0_20260922.1623.release` (and the same source built as
`w98link-debug`), Windows 98 SE on the A21m.

Found while using `sync --dry-run` to check a USB stick copy against the
A21m's C: drive. Two defects surfaced together; they are filed as one
because the first is only visible once the second is fixed. A third, hit on
the same run, is in `bug-sync-connect-timeout.md`.

## What happened

```
$ w98link sync --dry-run /run/media/vifair/G60SDFW/A21M/WIN98 C:/WIN98
8 file(s) here, 0 already there, 0 to send
dry run: nothing was changed
$ echo $?
0
```

Eight files, none the same and none to send, which cannot be right. The same
directory minus its one 62 MB file (`directx_dec2006_redist.exe`) gives the
expected answer:

```
7 file(s) here, 7 already there, 0 to send
```

A 5,771-file, 671 MB tree (an Office 97 source) did the same:
`5771 file(s) here, 0 already there, 0 to send`, exit 0.

Why these files went down the hash path at all: the stick was mounted on
Linux with vfat's default time handling, which reads FAT's local timestamps
as UTC. Every mtime was off by the local UTC offset, so the agent saw "same
size, different time" and hashed its copy to decide. A 25 MB tree whose times
lined up synced correctly in 6 s.

## Defect 1: the agent hashes on the link's thread, and a slow hash kills the session

`sync_entry` in `src/core/agent.c` calls `wl_sha256_file` synchronously while
it handles a manifest batch. Nothing pumps the link while it runs. Once the
agent goes `DEAD_MS` (10 s, `src/core/link.c:12`) without servicing the link,
the link is declared dead and the session restarts. The client sees a new
session and fails the sync.

Caught in gdb on the client (`build/bin/w98link-debug`, `break fail_msg`):

```
fail_msg (..., msg="link reset during the sync", n=26) at src/core/sync.c:73
#1 on_session  src/core/sync.c:126
#2 new_session src/core/peer.c:497
#3 wl_peer_work src/core/peer.c:511
```

A Pentium III hashing from its own IDE disk cannot get through 62 MB in 10 s,
and a manifest batch of many smaller files that all need hashing adds up the
same way. That the hash is what stalls the link is inferred, not measured:
the file boundary above (with it fails, without it passes) is the evidence.
A timestamped log line around `wl_sha256_file` on the agent would confirm it.

Fix directions, not mutually exclusive:

- Hash incrementally: hash a bounded chunk, pump the link, continue. The
  agent already runs a pump loop, so a per-sync hashing cursor fits the
  existing shape better than a thread.
- Or hash on a worker thread and answer NEED when it finishes. Win9x threads
  are fine for this; the agent already uses them for the process pipes.
- Either way, the link should keep up keepalives while any long operation
  runs. The same starvation could hit `put`/`get` if a disk write or read
  ever blocks for 10 s (a floppy or a spun-down disk).

## Defect 2: a failed dry run exits 0 and prints a plausible summary

`cmd_sync` in `src/client/main.c:604-609` prints the counts and, for a dry
run, returns 0 before looking at `r.ok` or `r.message`. So a failed sync
under `--dry-run` reports "nothing was changed" and success, with counts
that add up to nothing. This is the more dangerous defect, because a dry run
is exactly what someone uses to check before deleting things.

Fix: check `r.ok` before the dry-run branch, print `r.message`, and exit 1.
A test that fails a dry-run sync (for example, a link reset mid-sync in the
test harness) and asserts a non-zero exit would have caught it.

## Reproducing

1. Mount a FAT copy of a target directory on Linux with the default vfat
   options, so every mtime differs from the target's, and include one file
   of 60 MB or more.
2. `w98link sync --dry-run <copy> <target dir>`: expect a bogus summary and
   exit 0 (defects 1 and 2).

## Test once fixed

- A dry run over a tree that needs hashing, with a 60 MB file, reports every
  file as already there and exits 0.
- A dry run whose link is reset mid-sync exits non-zero with a message.

## Fixed 2026-09-22

**Defect 1.** The agent no longer hashes while handling a record. A manifest
entry whose size matches but whose time does not is queued, and the serving
loop advances a hashing cursor by 256 KiB per iteration (`sync_hash_step`,
`src/core/agent.c`), so the link is serviced every few milliseconds however
large the file. The answer to the manifest goes out once hashing is done.

The inference in the report was right: with the slice limit removed, the new
regression test reproduces the failure exactly.

Regression test: `test_hashing_does_not_starve_the_link` gives the agent
100 MB to hash with the link set to declare death after 400 ms. It fails
with unbounded hashing and passes with the cursor. `wl_link_cfg` gained
`dead_ms` and `keepalive_ms` so a test can make the link impatient.

**Defect 2.** The exit status now comes from `wl_sync_exit_code`, which is
0 only when the sync succeeded and nothing failed, checked before the
dry-run branch. It is a function in the core so it can be tested directly
(`test_exit_code_is_not_success_when_a_sync_failed`).

## Found while fixing these

Adding fields to `wl_link_cfg` broke every caller that filled the struct
field by field: the new fields were stack garbage, and a stray `dead_ms`
made sessions reset at random. The fix is `wl_link_cfg_init` and
`wl_session_cfg_init`, which every caller now starts from, so a field added
later cannot do this again.
