# Iteration plan

Each iteration ends with something that runs, is tested, and is committed.
The protocol core is built and tested on Linux over a loopback link; the A21m
is only needed once there is an agent feature to try.

| # | Iteration | Done when |
|---|-----------|-----------|
| 0 | Skeleton: repo, Makefile, both builds, gates | `W98AGENT.EXE` built here runs on the A21m |
| 1 | Protocol design (`docs/protocol.md`) | reviewed and agreed before any protocol code |
| 2 | Framing: COBS, CRC32, frame format | unit tests incl. corrupted and truncated input |
| 3 | Transport interface, serial on both ends, pty loopback for tests | agent and client echo frames over the real cable |
| 4 | Reliability: sequence numbers, sliding window, ack/retransmit | survives a lossy simulated link in tests |
| 5 | Compression: vendored zlib, session-long streams, flush points | measured ratio on source trees; interactive latency OK |
| 6 | Channels and `exec` with streamed output | done: `w98link exec` runs commands on the A21m |
| 7 | `put` / `get` | done: files round-trip byte-exact; the agent updates itself |
| 8 | `sync`: hash manifests, send only changed files | done: 50-file tree, no changes, syncs in 0.25 s |
| 9 | TCP transport | same features over the new NIC |
| 10 | Windows 2000 pass | full suite against a 2000 agent, and the NT-only improvements below |

## Targets

**Retargeted 2026-09-22: Windows 2000 is the primary target.** The A21m is
being reinstalled with it (the machine repo tracks the disc and the driver
slipstream), because 98's DOS underpinnings cost this project real time: a
critical-error prompt in a DOS virtual machine freezes the whole agent, and a
command that goes through `command.com` cannot report an exit status. Neither
is true on 2000.

Windows 98 support is kept, not maintained. The 9x paths are already written
and cost nothing to carry, and dropping them would mean deleting working code
for no gain; but nothing is verified on 98 from here on, and where 98 and 2000
want different code, 2000 wins and the 9x side keeps whatever it has. What was
measured on 98 stays recorded in `Manual.md` section 10 and
`docs/bug-critical-error-hang.md`, because it explains why the code looks the
way it does.

What 2000 makes available, for iteration 10 (each needs a runtime check or
GetProcAddress, so the binary still starts on 98):

- `MoveFileEx(MOVEFILE_REPLACE_EXISTING)`, so `put` and `sync` replace a file
  in one step instead of remove-then-rename, which today leaves a window where
  neither the old nor the new file is in place.
- `SetHandleInformation`, replacing the `DuplicateHandle` dance that
  un-inherits the parent's pipe ends in `src/win32/proc.c`.
- Overlapped I/O on pipes, which would collapse the thread-per-pipe reader
  design into one waiter. Worth it only if the threads actually cause trouble.
- NTFS timestamps are UTC with 100 ns resolution, so the FAT local-time
  conversion and `WL_MTIME_SLACK` become unnecessary there. They must stay for
  FAT volumes, which 2000 still mounts.
- `cmd.exe` returns real exit codes, so the "run it directly when it has no
  metacharacters" rule in `wl_proc_start` stops being load-bearing. Keep it:
  it is still one fewer process.
- Possibly a faster line. Win9x's serial driver caps usefully at 115200; NT's
  will set higher rates on a 16550A, and the FTDI end has no trouble. Worth
  measuring before the NIC arrives, since it is free throughput.

The agent targets Win32 as shipped in Windows NT 4.0+ (2000, XP) and, without
active work, Windows 98 SE. Watcom's `-bt=nt` output runs on all of them, so
what matters is API discipline in `src/win32/`:

- ANSI (`*A`) entry points only. Win9x has almost no Unicode API, and the
  ANSI calls work unchanged on NT.
- Only APIs present on both 98 and NT 4.0. Toolhelp32 is out (NT 4.0 lacks
  it), and so is anything NT-only, such as ReadDirectoryChangesW or
  CreateProcessWithLogonW. Where a newer API would help, load it with
  GetProcAddress and keep a fallback.
- Overlapped I/O only on comm and socket handles: Win9x does not support it
  for disk files.
- The handshake reports the platform (GetVersionEx dwPlatformId) so the
  client can adapt, e.g. cmd.exe vs command.com for `exec`.

Names in this repo still say 98: the project directory, `make win98`,
`W98AGENT.EXE`, `include/w98link/`. They all change in one mechanical commit
when the project gets its real name, which has not been chosen yet; renaming
twice would be worse than carrying a wrong name for a while.

Bootstrapping: the first agent binary reaches the A21m over the existing
Kermit link. From iteration 7 on, the agent can replace itself. After the
Windows 2000 reinstall the first copy comes from a USB stick instead, which
2000 mounts without a driver hunt.
