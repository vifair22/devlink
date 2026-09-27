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
| 9 | TCP transport, encrypted, served beside serial | same features over the new NIC, several sessions at once |
| 10 | Windows 2000 pass | full suite against a 2000 agent, and the NT-only improvements below |
| 11 | Local multiplexer | several commands at once from separate terminals, over one session |
| 12 | A window instead of a console | the agent runs as a real Win32 window on the A21m, showing what it is doing now |

## Iteration 9: TCP, encrypted, beside serial

Decided 2026-09-27; `docs/protocol.md` has the wire format ("Link layer:
TCP", "Sessions and transports"). The user's choices:

- **Both at once.** The agent serves serial and TCP together, and any number
  of sessions, the way the local multiplexer serves several clients over one
  line. Each session is answered over the connection it came in on.
- **Encrypted from the first release.** Noise `NNpsk0` with a key from a
  pre-shared passphrase: no certificates, forward secrecy, ChaCha20-Poly1305.
  TLS was considered and turned down for its size and its certificates, not
  for the Pentium III's speed.
- **No passphrase on serial.** The cable is physical access.
- **Passphrase files:** `DEVLINK.KEY` beside the agent, `~/.config/devlink/key`
  or `$DEVLINK_KEY` on the client. The agent will not listen without one.
- **The agent's window shows TCP:** the listening port and each connected
  client's address.

The network card for the A21m is delayed in shipping, so everything up to the
last step is proved on Linux over 127.0.0.1, and on Windows 2000 in QEMU with
a virtual NIC (the runbook's image, started with `-nic user` instead of
`-nic none`).

Steps, each committed only when the gates pass:

| Step | Work | Done when |
|------|------|-----------|
| 9a | A link interface in `session.c`: the serial link becomes one implementation of a small table of operations (send, recv, is_up, generation, unacked, pump) | every existing test passes unchanged; no behaviour change |
| 9b | The TCP link: `u16`-length messages, keepalive and 10 s dead timer, unencrypted for now and only reachable from tests | sessions run over a socketpair; the timers hold to the millisecond in virtual time. (`exec` over TCP moved to 9e: its tests open one session after another on a line, and over TCP each needs a new connection, which only an accepting agent has) |
| 9c | Noise: vendored Monocypher, HMAC and PBKDF2 over the existing SHA-256, the `NNpsk0` handshake and transport messages | Noise's published vectors for `Noise_NNpsk0_25519_ChaChaPoly_SHA256` pass; tampered and wrong-key sessions fail as specified |
| 9d | Transports and addressing: POSIX TCP connect for the client, Winsock 1.1 listen/accept for the agent (`wsock32`, present on 98 and 2000), `DEVLINK_LINE=tcp:HOST[:PORT]`, the multiplexer naming its socket for a TCP target | the Linux client runs `exec`, `put`, `get` and `sync` against the Linux agent over TCP |
| 9e | Several sessions in the agent: serial and every TCP connection served at once, EXEC slots shared, `@stop` ending all of them | a test holds a serial session and two TCP sessions open, runs a command on each, and each output arrives whole on its own connection only; the `exec` suite passes over TCP as well as serial |
| 9f | The window: TCP listener and connected clients | shown on the A21m (or the 2000 VM) |
| 9g | Measure and fix the PBKDF2 iteration count on the A21m; set the TCP deflate level from measured throughput | numbers recorded here; the protocol's figure made final |
| 9h | On the A21m, once the card is fitted | the full suite over TCP, beside a serial session |

Open for 9e, to decide when it is built: how the Win32 agent waits on serial
and sockets together. `select()` covers only sockets there, and a serial
handle cannot join it; the likely shape is one reader thread per connection
posting to the main loop, as the pipe readers already do, but that is to be
settled in the code, not here.

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

**What it has cost since.** Three defects, all found in use rather than by the
tests, and all the same shape: something handled on one side only. A channel
number held because the agent's CLOSE was answered without sending ours. A
deferred close with a comment describing it and no code to finish it. Then a
record dropped, which is the serious one: a large, highly compressible file
through the multiplexer arrived 4 KiB short and failed its checksum, because
records refused by the far side were kept in a stash of one and the next one
overwrote it (`docs/bug-mux-drops-a-record.md`). Plain transfers were never
affected.

The pattern is worth naming. In each case a comment stated the invariant
correctly and nothing enforced it. The backlog that replaced the stash is
bounded by something provable rather than assumed, and overrunning it now ends
the channel with a message instead of losing data quietly.

## Iteration 12: a window instead of a console

Decided 2026-09-23. The agent is already a Win32 console PE; this makes it a
Win32 *window*. The reason is not cosmetic. Since iteration 11 the agent can
have eight commands, four transfers and a sync live at the same time, and a
scrolling log fundamentally cannot show what is happening **now**, only what
happened. A window can hold a list of live channels next to the log, which is
the one view the console can never render.

The seams for it were already there, put in for the console:
`wl_agent_cfg.stop` is polled once per loop iteration and `wl_agent_cfg.log`
is called for everything worth printing. So `wl_agent_serve` runs unchanged on
a worker thread, the GUI thread runs an ordinary message pump and never blocks
on the port, and Stop stays cooperative, bounded by one 20 ms pump, with no
TerminateThread anywhere near it.

Two binaries, from one set of objects. `DEVLINK.EXE` stays exactly as it is:
the console agent, the diagnostics (`echo`, `linktest`), and the unmaintained
9x path. `DEVLINKW.EXE` is the window, linked `-l=nt_win`. The reason is
Windows 2000, not taste: `AttachConsole` is XP and later, so a GUI-subsystem
binary on 2000 cannot write to the console that launched it, and a single GUI
binary would silently lose the output of every diagnostic. Same split as
vim/gvim.

The window itself has to stay dumb, because nothing in it can be reached from
cmocka on Linux: it creates controls, lays them out on WM_SIZE, and routes
messages. Everything with logic goes below it into `wl_monitor`, a fixed-size
model in the core with the log ring and the live-channel rows, fed by a new
`wl_agent_cfg.event` callback carrying the structured facts (session up, client
identified, channel opened, progress, closed) that `log` currently flattens
into prose. That callback is tested the way the rest of the agent is: the real
loop in a thread over a pty, running real commands, asserting a row appears
while a command runs and is gone after it ends. The GUI owns a CRITICAL_SECTION
around the producer callback and renders on a 250 ms timer, which coalesces
bursts and keeps a 500 MHz P3 from repainting a ListView per event.

One trap, found by reading rather than by running it: `wl_proc_start` passes
`dwCreationFlags = 0`, so children inherit the agent's console. A
GUI-subsystem process has none, and Windows would then allocate a fresh
console per command, flashing a black box on the target every time anything
runs. The GUI build needs `CREATE_NO_WINDOW` on NT, or a hidden
`STARTUPINFO` on 9x.

Order of work: the model and the event callback first, tested and with nothing
Windows-specific in it; then the window, the worker thread and the layout;
then the parts that make it an agent rather than an app, which is the tray
icon, starting minimised, and replacing the batch file in Startup; then the
manual, the `dist` layout and a deployment once it is cleared.

**Done so far.** `wl_monitor` and the `event` callback, with eleven tests: ten
on the model and one that runs the real agent loop over a pty and requires a
command to be listed while it runs and gone after it ends, checked against an
agent with each half of the reporting removed. Then `DEVLINKW.EXE` itself: a
worker thread, a virtual list answered from the model as it paints, a log pane
appended to rather than rebuilt, and a status bar. `wl_proc_start` now passes
`CREATE_NO_WINDOW` with a hidden `STARTUPINFO`, which was the trap named above.

**Looking at it without the target.** `make gui-preview` runs the window under
Wine on a headless X server and converts the framebuffer to a PNG
(`tools/gui-preview.sh`, `tools/xwd2png.py`). It is in no gate and skips itself
where Wine or Xvfb is missing. What it proves is that the window comes up and
the layout holds, against Wine's common controls rather than Windows 2000's.
Driving it from the real client over a socat pty pair, which the same rig
allows, is what showed the list working with a command and a transfer running
together, and turned up two defects elsewhere (see below).

**Then the rest of the window.** An icon, drawn by `tools/make_icon.py` at
build time and bound into both binaries with a version block, so the file says
which build it is when there is no client to ask. A tray entry with Open,
Start, Stop and Exit, declared at the size shell32 4.0 used because asking for
a larger structure than the shell understands fails outright. Minimising goes
there and off the taskbar; `-tray` starts it there, which is what a Startup
shortcut wants. A File and Help menu, and Copy line, Copy all and Clear on the
log, because the message worth having is usually an error and the alternative
is reading it off the target's screen.

**Replacing itself.** AGENT.BAT restarts the console agent and swaps the
upload in once it has exited; a windowed program cannot borrow that without
keeping a console on the desktop. It does the swap itself instead, and the
trick that makes it possible without a helper program is that Windows will not
let a running image be written over or deleted but will let one be renamed
within the same volume. On `@stop`, once the loop has finished and the port is
free, it steps aside as `DEVLINKW.OLD`, moves `DEVLINKW.NEW` into place and
starts it with the command line it was given, so the port, the rate and
`-tray` survive. The successor deletes the old build once it has finished
exiting, and the next update clears it if that did not work; either way it is
the way back from a bad build. An `@stop` with nothing waiting is just a stop.

The path arithmetic is the part that can be wrong on its own, so it is
`wl_path_sibling` in the core with tests, rather than a `strrchr` at the call
site that treats a dot in a directory name as an extension.

Proved under Wine on a headless display: two builds with different ids, the
update applied by `@stop`, the successor answering as the new build with the
old one cleaned up; the same again started with `-tray`, still hidden
afterwards; and `@stop` with nothing waiting leaving the window up and
`Start` available. What that does not prove is the permission itself, since
renaming an open file is something Linux always allows. On NT it is the image
section that pins the contents, not the name.

**Still to do.** The Startup shortcut on the target, and a deployment, which
is the user's to clear.

### Two defects found while looking at the window

Neither is in the window. Both were in the way of running it here, which is how
they turned up.

- **The busy message named the wrong process.** `name_the_holder` walked /proc
  for anything with the port open and reported the first match, which is not
  necessarily the one holding the lock: here it named socat, which was passing
  the bytes through, while the multiplexer held the flock. Sending somebody
  after the wrong pid is worse than naming nobody, since they may kill it. It
  now reads `/proc/locks`, which answers the question outright, and falls back
  to the scan for an opener that took the port without locking it.
- **The multiplexer ran as `exe`.** It starts by `execv("/proc/self/exe")`,
  which is deliberate: it guarantees the daemon is this exact binary rather
  than whatever now sits at that path. The kernel takes the process name from
  the file executed, though, so the daemon appeared as `exe` and neither `ps`
  nor `pkill devlink` found it. A `prctl(PR_SET_NAME)` restores the name
  without giving up execing the running image.

## The Linux agent

Added 2026-09-25 for devlink-rescue (git.airies.net/vifair22/devlink-rescue),
a small Linux image for 32-bit-era machines that will not boot their own OS.
The agent core was already platform-neutral and the suite already ran it as a
Linux peer, so the port is an entry file (`src/linux/main.c`) and a static
build target (`make agent-linux`, with `CC` and `LINUX_AGENT_ARCH` for the
image's i586 toolchain). Two changes to the POSIX layer came with it:

- **Cancel kills the process group.** Commands get a session of their own and
  `wl_proc_kill` signals the group while the shell is unreaped. Found by
  writing the test first: a cancelled `(sleep 2; touch M) | cat` still made
  its marker.
- **The agent build may set the clock** (`-DDEVLINK_AGENT`). Every other POSIX
  build still refuses, so no test can move a developer's clock. System clock
  only; the hardware clock is left to `hwclock --localtime`, since Windows
  keeps it in local time.

Checked end to end with the real binaries over a socat pty pair: info, exec
with stdin and exit status, a 200 KB put, @time, and @stop.

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

The rename to devlink landed in `d62528e`, so the names in the repo are the
real ones. One is left: the working directory on the development machine is
still `w98link`, which is cosmetic and costs a `git remote` check to change.

Bootstrapping: the first agent binary reaches the A21m over the existing
Kermit link. From iteration 7 on, the agent can replace itself. After the
Windows 2000 reinstall the first copy comes from a USB stick instead, which
2000 mounts without a driver hunt.
