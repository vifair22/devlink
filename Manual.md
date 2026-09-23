# w98link Manual

A compressed, reliable link between a Linux workstation and a Windows 2000 or
NT machine, over a serial cable now and over TCP later. This manual covers
installation, operation, tuning, diagnosis and the limits of the thing.

For the wire format see `docs/protocol.md`; for what is built and what is not,
`docs/plan.md`.

Contents:

1. What it is
2. Requirements
3. Building
4. Installing on the target
5. Running the agent
6. Client commands
7. The development loop
8. Performance
9. Diagnosing a link
10. Limits and known behaviour
11. Maintenance
12. Error codes

---

## 1. What it is

Two programs:

| Program | Runs on | Purpose |
|---------|---------|---------|
| `w98link` | Linux | the client: you type commands to it |
| `W98AGENT.EXE` | Windows 2000 (also NT 4.0 and 98 SE) | the agent: it serves one client at a time |

Both are built from the same protocol core, so both ends always agree on the
format. The client always starts the conversation; the agent never calls out.

What it does:

- runs commands on the target and streams their output back, with exit codes
- copies files in either direction, checked with a CRC
- syncs a directory, sending only what differs
- compresses everything on the wire, which is what makes a 115200 bps line
  usable for development

What it does not do, by design in v1: authentication, encryption, more than
one client at a time, and resuming an interrupted transfer. See section 10.

## 2. Requirements

**Workstation:** Linux, a USB or built-in serial port, and read/write access
to it (membership of the `uucp` group on Arch, `dialout` on Debian).

**Target:** Windows 2000, the platform this is developed against. NT 4.0 and
Windows 98 SE also run it, the second without any current verification; where
this manual says something is true of 98 it was measured there. A free COM
port. About 250 KB
of disk for the agent.

**Cable:** a null-modem cable between the two serial ports. A straight-through
cable will not work; both ends are DTE. If bytes never arrive in either
direction, suspect the cable first (section 9).

**To build:** gcc, make, cmocka, gcovr, cppcheck and clang-tidy for the Linux
side; Open Watcom v2 for the agent. Open Watcom is not packaged: unpack the
upstream Linux release into `~/.local/opt/watcom`, or point `WATCOM=` at it.

## 3. Building

```sh
make            # the client       -> build/bin/w98link
make win98      # the agent        -> build/bin/W98AGENT.EXE
make test       # the whole suite
make coverage   # line coverage, gated at 80%
make analyze    # cppcheck, gcc -fanalyzer, stack usage
make tidy       # clang-tidy
make debug      # or: make asan
make help
```

The agent is built for the Win32 API as it exists in Windows NT 4.0 and, so
that one binary serves both, Windows 98:
ANSI entry points only, no function that either lacks, and no overlapped I/O
on pipes. It runs on everything from 98 to Windows 11.

Vendored zlib lives in `third_party/zlib` and is built with its own flags,
outside this project's warnings and coverage gates. Its provenance and
checksum are in `third_party/zlib/VENDORED.md`.

## 4. Installing on the target

The agent is one executable plus a launcher batch file. Getting the first copy
across needs some existing channel: a USB stick, a network share, or Kermit.
After that the agent updates itself over its own link (section 11).

1. Copy `build/bin/W98AGENT.EXE` to a directory on the target, for example
   `C:\W98LINK`.
2. Create `C:\W98LINK\AGENT.BAT`:

   ```
   @echo off
   cd \W98LINK
   :top
   W98AGENT serve COM1 115200
   if exist W98AGENT.NEW del W98AGENT.EXE
   if exist W98AGENT.NEW ren W98AGENT.NEW W98AGENT.EXE
   goto top
   ```

   The loop restarts the agent when it exits, and swaps in a newly uploaded
   binary if one is waiting. Write the file on the target, or copy it with a
   tool that does not alter line endings: a batch file with doubled carriage
   returns makes `command.com` report "Bad command or file name" for lines it
   cannot parse.

3. Run `AGENT.BAT`. It prints its version and then a line per client and per
   command it runs.

**Which COM port.** Whatever the target's Device Manager calls the port the
cable is in. The agent takes the name as its first argument. If another
program holds the port (HyperTerminal, a Kermit server, a mouse driver), the
agent will say so and exit.

## 5. Running the agent

```
W98AGENT serve [PORT] [BAUD]      defaults: COM1 115200
W98AGENT --version
W98AGENT echo [PORT] [BAUD] [IDLE_SECONDS]
W98AGENT linktest [PORT] [BAUD] [WAIT_SECONDS]
```

`serve` is the agent proper and runs until stopped. `echo` and `linktest` are
diagnostics (section 9).

**Stopping it.** Ctrl-C stops the agent, and the launcher loop starts it
again; answer Y at "Terminate batch job" to stop the loop as well, or close
the window. From the workstation, `w98link exec @stop` stops the agent so the
launcher can restart it, which is how an update is applied.

**Console output.** Each line is timestamped: sessions starting, commands
run, transfers, and failures. It is the first place to look when the client
reports something odd.

## 6. Client commands

Every command takes `--line` and `--baud`. `--line` defaults to
`/dev/ttyUSB0`, or `$W98LINK_LINE` if that is set; `--baud` defaults to
115200. Setting `W98LINK_LINE` once is the convenient way:

```sh
export W98LINK_LINE=/dev/serial/by-id/usb-FTDI_FT232R_USB_UART_AG0JT9KH-if00-port0
```

The `by-id` path is worth using: `/dev/ttyUSB0` moves when another USB serial
device is plugged in first.

### exec

```
w98link exec [--cwd DIR] [--merge] [--] COMMAND...
```

Runs COMMAND on the target through its shell and streams stdout and stderr
back as they are produced. The words are joined with spaces and parsed by the
target's shell, so its quoting rules apply, not the client's.

```sh
w98link exec ver
w98link exec dir C:\W98LINK
w98link exec --cwd C:/SRC wmake all
w98link exec --merge nmake            # keep errors in step with output
```

`--` ends the client's own options, which matters when the remote command has
options of its own that would otherwise be read as the client's.

**Exit status:** the client exits with the command's exit status, or 125 if
the command could not be run at all (the agent refused it, or the link
failed). Read section 10 on exit codes under Windows 9x, which has an
important caveat.

**Input:** v1 gives the command an empty stdin. A program that reads input
sees end of file at once rather than hanging.

**Drives that are not ready.** A command naming a drive letter that has no
media in it is refused before it starts, with `drive D: is not ready` and exit
status 125. This is not tidiness: on Windows 9x such a command stops inside DOS
on a prompt that freezes the whole agent (section 10), so it must never be
started. Letters are read from the whole command line, including arguments, and
a letter joined to the word before it (`prod:`) is not treated as a drive. Put
a disc in and the same command runs.

**Interrupting a command (Ctrl-C).** Ctrl-C does not simply kill the client:
it sends a cancel to the agent, which kills the command over there and
acknowledges, and only then does the client exit, with status 130. This
matters because a command abandoned on the target keeps whatever it had open,
and on Windows 9x that can be a drive or a file the next transfer needs.

```
$ w98link exec 'dir D:\'
^C
w98link: cancelling on the target...
w98link: cancelled; the command was killed on the target
$ echo $?
130
```

The agent answers a cancel immediately rather than waiting for the process to
die, so the command's remaining output and its real exit status are discarded;
a command that will not die (see the DOS VM note in section 10) is released in
the background while the agent goes on serving. If the acknowledgement does
not arrive within about three seconds the client says so and exits anyway, and
a second Ctrl-C ends it at once.

Two things cancel cannot promise. It kills the command the agent started, so a
program that spawned children of its own may leave them behind. And it cannot
rescue a command stopped at a DOS critical-error prompt: while that prompt is
up, Windows 98 freezes the agent itself, so the cancel is not read until
somebody answers the prompt at the target's keyboard. See section 10.

**Agent commands:** a command starting with `@` addresses the agent itself
rather than the shell. There are two.

`@stop` makes the agent exit with status 0, so its launcher can put a newly
uploaded binary in place and start it again. This is how the agent is updated;
see section 3.

`@dir PATH` lists one directory. It is handled inside the agent, so unlike
`dir` it never starts a DOS virtual machine, and it reports what is actually on
disk:

```
$ w98link exec '@dir C:\W98AGENT'
f 135168 1790116008 W98AGENT.EXE
f 423 1790103092 AGENT.BAT
```

Type (`f` file, `d` directory), size in bytes (0 for a directory), mtime as
seconds since 1970-01-01 UTC, then the name, one space between each. The name
is the rest of the line, so a name containing spaces needs no quoting on this
side; split the line three times and stop. Entries are in directory order, not
sorted, and `.` and `..` are left out. A path that cannot be listed fails with
status 125 and a message rather than printing nothing.

`@time` reads the target's clock, and `@time set SECONDS` sets it. Use the
client's `time` command rather than this directly.

`@caps` reports what the serial port underneath can do, asked of the open
handle so nothing is changed:

```
$ w98link exec '@caps'
transport serial (win32)
current 115200 8N1
subtype RS232
buffers rx 16384 tx 0
maxbaud any (BAUD_USER)
settable 75 110 ... 57600 115200 any (BAUD_USER)
```

Ask this before changing how the line is driven. A settable list that stops at
115200 with `any (BAUD_USER)` after it, which is what the A21m reports, means
the driver will not promise a ceiling and will only answer for a specific rate
when asked. On that machine the answer is arithmetic: a 16550A clocked at
1.8432 MHz divides to exactly 115200 at a divisor of 1, and 1 is the smallest
divisor there is, so 115200 is the ceiling. Faster rates need a Super I/O
high-speed mode that the stock driver does not enable.

Compared with `dir` on the same directory: long names instead of 8.3 aliases,
exact byte sizes, a machine-readable time instead of a locale-formatted one, a
real exit status, no volume header or free-space footer, and about half the
time (97 ms against 182 ms on the A21m). Use `dir` when you want the DOS
report; use `@dir` when something on this side has to read it.

### time

```
w98link time [--line PATH] [--baud N] [--set]
```

Compares the two clocks, and with `--set` corrects the target's from this one.

```
$ w98link time
workstation 2026-09-23 05:20:11 UTC
target      2026-09-23 05:17:43 UTC
difference  2 m 28 s behind
target tz   UTC+0:00
round trip  47 ms

run 'w98link time --set' to correct it; sync compares file times
```

**Why this matters more than it looks.** `sync` decides what to send by
comparing modification times, so a target whose clock is wrong either resends a
whole tree every time or, if its clock runs ahead, writes files that look newer
than the originals and then misses real changes. The A21m's RTC drifts minutes a
day, and a machine whose CMOS battery has died starts somewhere in the last
century, so this is worth running after every boot until the battery is
replaced.

**What is set is UTC**, not local time. The protocol's file times are UTC, so
that is what has to be right; if the target's time zone is set wrongly its
clock will read wrong on screen while everything the link does stays correct.
The reported `target tz` is there to show you that separately.

**Accuracy** is within the round trip, tens of milliseconds on a serial line,
against a two-second tolerance in `sync` for FAT's timestamp resolution. No
correction for latency is applied, because none is needed at that ratio.

Setting the clock while a session is running is safe. The link's timers are
monotonic, not wall clock, which is exactly what was not true of Kermit: moving
the clock under Kermit killed the session, and that is why this project's
earlier notes say never to change the time mid-transfer. That caveat no longer
applies.

### put and get

```
w98link put [--force] LOCAL REMOTE
w98link get REMOTE LOCAL
```

Copies one file, compressed, with a CRC-32 over the whole file. The file is
written under a temporary name and renamed once it has been verified, so an
interrupted transfer never leaves a half-written file where the real one
should be. The modification time travels with the file.

`put` refuses to overwrite an existing file unless `--force` is given.
Options may appear before or after the paths.

```sh
w98link put build/app.exe C:/SRC/app.exe --force
w98link get C:/SRC/output.log ./output.log
```

Remote paths may use `/` or `\`; `/` is usually easier, since a shell on the
workstation treats `\` as an escape.

### sync

```
w98link sync [--delete] [--dry-run] [--hidden] LOCALDIR REMOTEDIR
```

Makes REMOTEDIR on the target match LOCALDIR, sending only what differs. This
is the command to use in a development loop.

How a file is judged:

1. Not there, or a different size: it is sent.
2. Same size and a modification time within two seconds: it is not sent.
   (Two seconds because FAT stores times at that resolution.)
3. Same size, different time: the agent hashes its copy and compares it with
   the client's. Only if the contents differ is the file sent; if they match,
   the agent lines its timestamp up so the next sync takes the cheap path.

Names starting with a dot are skipped unless `--hidden` is given, which is
what keeps `.git` out of a sync.

`--delete` removes files on the target that the manifest does not list.
Deletions happen only after the whole manifest has arrived, so an interrupted
sync never deletes anything. `--dry-run` reports what would happen and
changes nothing.

```sh
w98link sync ./src C:/SRC/src
w98link sync --delete --dry-run ./src C:/SRC/src   # look before leaping
```

Sync creates missing directories on the target, including nested ones.

### info

```
w98link info
```

Connects, prints the agent's build version, platform and protocol version,
and disconnects. The quickest check that the far end is alive, and the way to
confirm an update took effect.

### echo and linktest

Diagnostics; see section 9.

## 7. The development loop

With the agent running, a compile-and-run cycle is three commands:

```sh
export W98LINK_LINE=/dev/serial/by-id/usb-...-if00-port0

w98link sync ./src C:/SRC/src        # push what changed
w98link exec --cwd C:/SRC wmake      # build on the target
w98link get C:/SRC/app.exe ./app.exe # fetch the result, if you want it
```

Put that in a shell function or a Makefile target. A sync of an unchanged
50-file tree takes about a quarter of a second, so running it every time
costs nothing.

**Watch the agent's console** on the target while you work: it logs every
command and transfer, which makes a failing step obvious.

## 8. Performance

Measured on a ThinkPad A21m (Pentium III, Windows 98 SE) over a null-modem
cable at 115200 bps, with an FTDI USB adapter on the workstation:

| What | Figure |
|------|--------|
| Raw line rate | 11,520 bytes/s each way |
| Link throughput, both directions at once | 11,244 bytes/s each way (98%) |
| Retransmissions on a good cable | none observed |
| C source, compressed | about 4x, so roughly 34-40 KB/s effective |
| Already-compressed data (zip, exe) | roughly the raw rate |
| `put` of a 120 KB binary | about 7 s |
| `sync`, 50 files, 240 KB, first time | 5.9 s |
| `sync`, nothing changed | 0.25 s |
| `sync`, one file edited | 0.32 s |

Compression is what makes this usable: on source code the line behaves as if
it ran at roughly 350,000 bps.

**Speed.** 115200 is the ceiling for a standard PC serial port; the chip
cannot go faster with the stock Windows driver. Lower speeds work if a cable
or port is marginal: pass `--baud` to the client and the same figure to the
agent. Both ends must agree.

## 9. Diagnosing a link

Work from the bottom up. Each tool tests one layer, and each needs the agent
started in the matching mode, so stop `serve` first.

**1. Bytes move at all.** On the target run `W98AGENT echo COM1 115200 120`,
then on the workstation:

```sh
w98link echo
```

It sends 20 frames and checks each one comes back. All echoed means the
cable, both ports and the framing are sound. Nothing echoed means the cable
(a straight-through cable instead of a null-modem one is the usual cause),
the wrong port, the wrong speed, or another program holding the port.
Mismatched or corrupt echoes mean a marginal cable or wrong speed.

**2. The reliable layer.** On the target run `W98AGENT linktest COM1 115200`,
then:

```sh
w98link linktest                       # incompressible data: the raw rate
w98link linktest --file src/core/link.c  # real data: the compressed rate
```

It reports bytes echoed, corruption, frames sent and resent, CRC errors and
the achieved rate. A handful of resends is normal on a long cable; a rate far
below 11 KB/s with many CRC errors means a noisy line, and a lower `--baud`
on both ends will do better than a fast one that keeps failing.

**3. The protocol.** With `serve` running:

```sh
w98link info
```

If `echo` works but `info` does not, the problem is above the link: usually a
client and agent from different builds. The version mismatch shows in the
agent's console.

**4. A specific operation.** Set `W98LINK_DEBUG=1` to trace a sync's
exchange on stderr:

```sh
W98LINK_DEBUG=1 w98link sync ./src C:/SRC/src
```

**Things that have actually gone wrong here**, and what they looked like:

| Symptom | Cause |
|---------|-------|
| "Bad command or file name" from a batch file | the file was copied by something that doubled its carriage returns. Copy it with `put`, which is byte-exact |
| A command's exit status is always 0 | the command went through `command.com`; see section 10 |
| The agent will not start: "cannot open COM1" | another program holds the port |
| `drive D: is not ready` | the command named a drive with no media in it; the agent refused it rather than letting DOS freeze (section 10) |
| A command prints `Abort, Retry, Fail?` and never returns | DOS reached empty media by a route the drive check could not see. Nothing on the client can help: answer the prompt at the target, or end the DOS task there. See section 10 |
| Everything stalls, then recovers after ten seconds | the link declared the peer dead and resynchronised; look for a loose connector |

## 10. Limits and known behaviour

**Exit codes on Windows 9x.** `command.com` always exits with 0, whatever the
program it ran returned. The agent therefore runs a command directly when it
contains no shell metacharacters (`| & < > ^ ( ) % "`), which preserves the
real exit code. A command that needs the shell reports 0 on Win9x whatever
happens. Practical rule: run the compiler directly rather than through a
redirect if you care about its status. NT's `cmd.exe` does not have this
problem.

**Shell builtins run in a DOS VM.** The agent itself is a 32-bit Win32
console program, but `command.com` on Windows 9x is 16-bit DOS running in a
virtual machine, and every command that needs the shell goes through it: the
builtins (`dir`, `copy`, `cd`, `type`, `set`), redirection, pipes, and
anything using `%` variables. Inside that VM neither the exit code above nor
the agent's error mode applies, so a builtin that touches a drive with no
media raises DOS's own `Abort, Retry, Fail?` prompt on the target's screen
and waits there forever:

```
$ w98link exec 'dir D:\'          # D: is the CD drive, empty
General failure reading drive D
Abort, Retry, Fail?               # no output ever arrives; Ctrl-C the client
```

**While that prompt is up the agent is frozen, and only a keypress at the
target frees it.** Measured on the A21m: no client was answered for over two
minutes, and cancelling does not help, because the agent is not running at
that point; Windows 98 is holding it behind the DOS virtual machine. Worse, the
prompt travels down the redirected pipe rather than to the screen, so there may
be nothing visible to answer: end the DOS task from Close Program
(Ctrl+Alt+Del), or restart the agent.

Because there is no recovering from it, the agent prevents it: any command
naming a drive letter with no media is refused before it is started, in about
70 ms, and the agent goes on serving. The check is a 32-bit call that honours
the error mode, which is why it fails where DOS prompts. It covers the way this
is normally reached, a command line that names the drive; a program that opens
empty media by some other route can still do it, so the rest of this advice
stands. Prefer real programs over builtins:
use `w98link get`/`put`/`sync` instead of `copy`, and where a listing is
needed, a small program rather than `dir`. `SetErrorMode` at agent startup
covers the agent's own file operations and any Win32 child, so `get` and
`put` against an empty drive fail cleanly instead of prompting.

**Paths are ASCII.** Non-ASCII paths are refused rather than guessed at.
Paths may be up to 259 characters. Long file names work; case is preserved.

**No authentication or encryption.** Anyone who can reach the port can run
commands as whoever started the agent. Over a cable that is the person at the
keyboard. This matters more when the TCP transport arrives.

**One client at a time.** A second client's connection displaces the first,
whose commands are then killed. This is deliberate: it is how a restarted
client recovers from an abandoned session.

**Interrupted transfers do not resume.** They fail, and nothing is left
half-written; run the command again.

**Timestamps.** FAT stores local time at two-second resolution. Sync allows
for that. If the target's clock is wrong, sync still works, since it compares
the times it recorded rather than trusting either clock.

**Memory on the target.** The agent needs about 1.2 MB, nearly all of it
buffers: the mux's per-channel queues about 536 KB, compression about 300 KB,
the link's windows and queues 128 KB, and the compression layer's plaintext
queues 144 KB. Each running command adds up to 72 KB. That is comfortable on
a machine with 64 MB, and the buffer sizes are compile-time constants in
`include/w98link/` if a smaller target ever needs them trimmed.

## 11. Maintenance

**Updating the agent**, with the launcher from section 4:

```sh
make win98
w98link put build/bin/W98AGENT.EXE C:/W98LINK/W98AGENT.NEW --force
w98link exec @stop
sleep 7
w98link info          # confirm the new build version
```

The agent exits, the launcher renames the new binary over the old one and
starts it again. The whole cycle takes under ten seconds. If the new agent
fails to start, the launcher loops trying to run it; recover at the target's
keyboard.

**Updating the client** is a rebuild.

**Version mismatch.** Client and agent must speak the same protocol version,
which the handshake checks; they refuse to run otherwise. Different build
versions of the same protocol version work together, and the mismatch is
reported in the agent's console so a stale agent gets noticed.

## 12. Error codes

Reported by the client when the agent refuses an operation.

| Code | Meaning |
|------|---------|
| 1 | protocol version mismatch |
| 2 | malformed record |
| 3 | unknown channel, operation or agent command |
| 4 | flow-control credit exceeded (a bug on one side) |
| 256 | file not found |
| 257 | access denied |
| 258 | file exists (a `put` without `--force`) |
| 259 | disk full, or the file could not be written |
| 260 | checksum mismatch: the transfer was corrupted and nothing was kept |
| 261 | invalid path, or a directory where a file was expected |
| 262 | the process could not be started |

Client exit statuses: 0 on success; the remote command's status for `exec`;
130 when an `exec` was cancelled with Ctrl-C; 125 when `exec` could not run
the command at all; 1 for any other failure.
