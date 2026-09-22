# Bug: sync's connect timeout counts the local scan

Status: fixed 2026-09-22. Found 2026-09-22 against client `0.1.0_20260922.1623.release`,
on the same run as `bug-sync-hash-starves-link.md`.

## What happened

```
$ w98link sync --dry-run --hidden /run/media/vifair/G60SDFW/OFFICE97PRO C:/Office/OFFICE97PRO
w98link: no answer from the agent within 10 s
```

The local tree was 5,771 files, 671 MB, read from a USB stick with a cold
page cache. The run took 37 s of wall time and 14 s of CPU, nearly all of it
hashing. `w98link info` answered straight afterwards, so the agent was fine.
Rerunning the same sync from a warm cache (about 8 s of CPU) connected
normally.

## Cause

`wl_sync_run` in `src/core/sync.c` takes `start = wl_clock_ms()` at line 346,
then calls `wl_fs_walk`, which hashes every local file before anything is
sent. The connect deadline at line 393 is measured from that same `start`:

```c
} else if (wl_clock_since(start) >= connect_timeout_ms) {
```

So a local scan that takes longer than the connect timeout (10 s) leaves no
time to connect, and the first pump that finds the peer not ready gives up.
The error blames the agent for time the client spent on its own.

## Fix

Take a separate timestamp for the connect deadline after the walk returns.
Keep `start` for `out->elapsed_ms` if the reported time should include the
scan; it probably should, since that is what the user waited for.

Worth checking the other commands for the same pattern: any client command
that does local work before connecting and measures the connect deadline
from its own entry has the same bug.

## Test once fixed

- A sync whose local walk takes longer than the connect timeout still
  connects and completes. In the test harness, a fake clock that advances
  during the walk makes this deterministic without a large tree.

## Fixed 2026-09-22

`wl_client_sync` takes a second timestamp after the walk and measures the
connect deadline from it; `out->elapsed_ms` still covers the scan. The other
client commands do no local work before connecting, so none had the bug.

Regression test: `test_connect_timeout_excludes_the_local_scan` hashes 100 MB
with a 50 ms connect timeout, which fails without the fix.
