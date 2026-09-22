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
| 7 | `send` / `get` | round-trip files, including long names |
| 8 | `sync`: hash manifests, send only changed files | edit-build loop pushes only the delta |
| 9 | TCP transport | same features over the new NIC |
| 10 | NT pass | full suite against an NT 4.0/2000 agent (VM is fine) |

## Targets

The agent targets Win32 as shipped in Windows 98 SE and Windows NT 4.0+
(2000, XP). Watcom's `-bt=nt` output already runs on both, so what matters is
API discipline in `src/win32/`:

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

Bootstrapping: the first agent binary reaches the A21m over the existing
Kermit link. From iteration 7 on, the agent can replace itself.
