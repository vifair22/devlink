# Iteration plan

Each iteration ends with something that runs, is tested, and is committed.
The protocol core is built and tested on Linux over a loopback link; the A21m
is only needed once there is an agent feature to try.

| # | Iteration | Done when |
|---|-----------|-----------|
| 0 | Skeleton: repo, Makefile, both builds, gates | `DEVLINK.EXE` built here runs on the A21m |
| 1 | Protocol design (`docs/protocol.md`) | reviewed and agreed before any protocol code |
| 2 | Framing: COBS, CRC32, frame format | unit tests incl. corrupted and truncated input |
| 3 | Transport interface, serial on both ends, pty loopback for tests | agent and client echo frames over the real cable |
| 4 | Reliability: sequence numbers, sliding window, ack/retransmit | survives a lossy simulated link in tests |
| 5 | Compression: vendored zlib, session-long streams, flush points | measured ratio on source trees; interactive latency OK |
| 6 | Channels and `exec` with streamed output | done: `devlink exec` runs commands on the A21m |
| 7 | `put` / `get` | done: files round-trip byte-exact; the agent updates itself |
| 8 | `sync`: hash manifests, send only changed files | done: 50-file tree, no changes, syncs in 0.25 s |
| 9 | TCP transport | same features over the new NIC |
| 10 | Windows 2000 pass | full suite against a 2000 agent, and the NT-only improvements below |
| 11 | Local multiplexer | several commands at once from separate terminals, over one session |

## Iteration 11: running more than one command at a time

Decided 2026-09-23. The wire protocol already multiplexes: channels, per-channel
credit, round-robin between them, and eight exec slots in the agent. What
prevents two commands is above it. Every `devlink` invocation is a process that
opens the port, handshakes, runs one command and exits, and the agent serves one
session, so a second invocation *displaces* the first and kills whatever it was
running. That has cost a running install.

The fix is local and needs no protocol or agent change: one process owns the
port and keeps a single session, and each `devlink` invocation talks to it over
a Unix socket, getting its own channel. Two side benefits, neither small: the
per-command handshake disappears, and so does the per-command deflate stream, so
compression context survives across commands instead of restarting from nothing
on a line where that is the scarce resource.

**Concurrency only, no detach** (the user's choice, and it keeps this small): a
channel dies with its client, so Ctrl-C or a closed terminal kills the remote
command exactly as it does today. Keeping a command alive across client exits
would need job ids and a registry in the daemon, which was considered and turned
down when recovery was discussed.

Known cost: a bulk `put` and an interactive `exec` sharing 11 KB/s means the
`exec` feels slow. Round-robin makes that fair rather than starved; per-channel
priority is a later question, if it turns out to matter.

The foundation is proved before anything is built on it:
`test_two_commands_run_at_once_on_one_session` opens a slow command, opens a
quick one while it runs, and requires the quick one to finish first with each
channel's output whole and separate. It was checked against an agent given a
single exec slot, where it fails.

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

What 2000 made available, and what came of each. Anything kept has a runtime
check or a GetProcAddress, so one binary still starts on 98.

- **`MoveFileEx(MOVEFILE_REPLACE_EXISTING)`: done.** `put` and `sync` replace a
  file in one step on NT instead of remove-then-rename, which left a moment
  with nothing at the destination; an interrupted transfer there took the file
  the target already had with it. Resolved through GetProcAddress, with the old
  pair kept for 98. Where MoveFileEx exists, its failure is reported rather
  than retried with the pair: falling back would answer "the destination is
  locked" by deleting the destination.
- **`SetHandleInformation`: declined.** It would replace a working
  `DuplicateHandle` that runs on both platforms with a GetProcAddress, a second
  code path and the same behaviour. That is more code to read, not less. The
  note that prompted this said SetHandleInformation "would be simpler", which
  is true only if 98 is dropped, and it is not.
- **Overlapped I/O on pipes: declined for now.** It would collapse the
  thread-per-pipe readers into one waiter, which is a real simplification, but
  the threads cause no trouble: the case that used to hurt, a reader that will
  not come back, is handled by the reaper. Revisit only if something concrete
  argues for it.
- **`WL_MTIME_SLACK`: kept.** NTFS timestamps are UTC with 100 ns resolution,
  so exact comparison would work there, but the two-second tolerance costs
  nothing and the target may be mounting FAT volumes, where it is required.
  Removing it would buy accuracy nobody asked for at the price of false
  "changed" verdicts on a FAT drive.
- **`cmd.exe` exit codes: verified on the target.** `exit 3` returns 3 and a
  pipe still reports honestly, where 98 always said 0. The "run it directly
  when it has no metacharacters" rule in `wl_proc_start` stops being
  load-bearing but stays: it is still one fewer process.
- **A faster line: no.** `@caps` on the A21m reports a settable list stopping
  at 115200 with BAUD_USER after it, so the driver promises no ceiling and only
  answers for a specific rate. The arithmetic decides: 1.8432 MHz over 16 is
  exactly 115200 at a divisor of 1, the smallest a 16550 has, so 230400 would
  need half a divisor. 115200 is the hardware ceiling here.

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

Names in this repo still say 98: the project directory, `make agent`,
`DEVLINK.EXE`, `include/devlink/`. They all change in one mechanical commit
when the project gets its real name, which has not been chosen yet; renaming
twice would be worse than carrying a wrong name for a while.

Bootstrapping: the first agent binary reaches the A21m over the existing
Kermit link. From iteration 7 on, the agent can replace itself. After the
Windows 2000 reinstall the first copy comes from a USB stick instead, which
2000 mounts without a driver hunt.
