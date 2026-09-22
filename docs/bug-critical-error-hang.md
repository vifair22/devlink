# Bug: a critical-error prompt on the target hangs the whole agent

Status: closed. Fixed and verified on the target; see the two corrections at the end. Found 2026-09-22 against agent `0.1.0_20260922.1801.win98`,
client `0.1.0_20260922.1623.release`, Windows 98 SE 4.10.2222 on the A21m.

## What happened

```sh
w98link exec 'dir D:\'      # D: is the A21m's CD drive, no disc in it
```

The client got no output and was killed by an outer `timeout 30`. After
that, every new client failed:

```
$ w98link info
w98link: no answer from the agent within 10 s
$ w98link exec 'dir E:\'
w98link: no answer from the agent within 10 s     (exit 125)
```

The agent answered again only after someone at the A21m dealt with the
prompt on screen. Nobody recorded which prompt it was (the DOS "Not ready
reading drive D / Abort, Retry, Fail" or the Windows "no disk in drive"
dialog), or what the agent's console printed.

## Two defects, not one

### 1. The agent lets children raise critical-error prompts

Touching a drive with no media makes Windows 9x raise a critical error. By
default that becomes a prompt that waits for a person, and nothing times it
out. A program meant to run unattended is expected to turn that off.

The agent never calls `SetErrorMode`. `wl_proc_start` in `src/win32/proc.c`
calls `CreateProcessA` with creation flags `0`, so each child inherits the
agent's error mode, which is the default: prompt.

Fix: call `SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX)`
once at agent startup, before any command runs. The mode is inherited
through `CreateProcess` unless `CREATE_DEFAULT_ERROR_MODE` is passed, so it
covers every child as well as the agent's own file operations (`put`, `get`,
`sync` against a missing drive would hit the same prompt). With it set, the
failing call returns an error instead, and `dir` prints "not ready" or
similar and exits.

**UNVERIFIED:** whether `command.com` honours the inherited mode. It is a
16-bit DOS program running in a VM, and its INT 24h handler may prompt
regardless of the Win32 error mode. A Win32 child certainly honours it. If
`command.com` does not, the options are to start commands directly rather
than through the shell where possible, or to install a critical-error policy
for the VM (for example, a small TSR or `command.com` options that force
Fail). Check this first, because it decides how much of the fix is enough.

### 2. One stuck command takes the whole agent down

This is the bigger problem. The design already covers it on paper: a new
client displaces the old session and its commands are killed (Manual,
section 10, "One client at a time"). `wl_proc_free` calls `TerminateProcess`,
waits 2 s, then joins each reader thread for 2 s before `TerminateThread`.
So a displaced session should be torn down in about 8 s at worst, and the
next client should be answered.

That did not happen. The agent stayed silent to repeated clients until the
prompt was dismissed by hand. Candidates, none confirmed:

- `TerminateProcess` on a Win9x DOS VM that is sitting in a critical-error
  prompt does not terminate it, and something downstream waits on it
  without a bound.
- `TerminateThread` on a reader thread blocked in `ReadFile` on the pipe
  misbehaves on 9x.
- A system-modal critical-error box stalls the agent's own serial I/O or
  message handling.
- Teardown takes longer than the client's 10 s answer timeout, and each new
  client displaces the previous attempt before teardown finishes. This one
  alone would not explain why it never recovered.

Whatever the cause, the property to restore is: nothing a command does may
stop the agent answering a new client. Worth considering alongside the fix:

- Tear down a displaced session asynchronously, so a new HELLO is answered
  before the old command is confirmed dead.
- A client-side way to cancel a running exec (a CANCEL on the channel,
  mapped to `wl_proc_kill`), so a stuck command can be dealt with without
  starting a new session.

## Reproducing

1. Target with an empty removable drive (the A21m's `D:`).
2. `w98link exec 'dir D:\'`, then from another shell `w98link info`.
3. Watch the agent console on the target. Record which prompt appears and
   what the agent logs, then pick Fail and see whether the agent recovers
   by itself.

Running the agent under a debugger on the target, or adding a log line at
each step of `wl_proc_free`, will show where teardown stops.

## Test once fixed

- `w98link exec 'dir D:\'` with no disc returns promptly with a non-zero
  status and no prompt on the target.
- Start `w98link exec` on a command that never exits (a test program that
  loops forever; `pause` will not do, since v1 gives it end of file at once),
  then run `w98link info` from a second client. It must answer within its
  timeout.
- `w98link get D:/x ./x` with no disc fails cleanly rather than prompting.

## Fixed 2026-09-22

- `SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX)` at agent
  startup (`src/agent/main.c`), inherited by every child because
  CreateProcess is not given CREATE_DEFAULT_ERROR_MODE.
- Teardown no longer blocks the link. `wl_proc_reap` (`src/posix/proc.c`,
  `src/win32/proc.c`) kills a child and releases it a little at a time,
  returning 1 only when it is gone; the agent keeps a small reaper list and
  drains it once per loop. `TerminateThread` is gone: a reader still blocked
  because a grandchild holds the pipe now costs a leaked thread rather than
  a broken agent.
- Regression test: `test_abandoned_command_does_not_block_the_next_client`
  in `tests/test_exec.c` starts a long command, drops the client, and
  requires the next client to be served promptly.

## Verified on the target 2026-09-22, agent build 1904

Repro run against the fixed agent at `C:\W98AGENT`, D: empty:

```
$ w98link exec 'dir D:\'
General failure reading drive D
Abort, Retry, Fail?                       (client killed by timeout 30, rc 124)
$ w98link info                            (immediately after)
w98link: no answer from the agent within 10 s
$ w98link info
w98link: no answer from the agent within 10 s
$ w98link info                            (about 40 s after the repro)
agent    0.1.0_20260922.1904.win98
```

Two findings.

**`command.com` does not honour the inherited error mode.** The question
the previous section left open is answered: the prompt is raised by the DOS
VM's INT 24h handler, which is below Win32 and does not consult
`SetErrorMode`. A Win32 child does honour it, so `put`/`get`/`sync` against
an empty drive are covered; a shell builtin such as `dir` is not.

**Defect 2 no longer holds the agent down, but recovery is not immediate.**
Nobody touched the machine and the prompt was never dismissed by hand; the
agent answered by itself on the third client. Earlier behaviour was silence
until a person intervened. The remaining latency is the reaper working
through a DOS VM that `TerminateProcess` does not kill promptly, plus each
displaced client resetting the session. That satisfies the standing
requirement (short of the OS crashing, the remote side re-establishes and
continues) and does not warrant a state machine, but the first client after
a wedged shell command will time out once or twice.

## Still open

The command itself still hangs rather than failing, because the prompt is
inside a DOS VM we cannot reach. Options, in order of appeal:

1. Answer the prompt for it. DOS reads the critical-error response from the
   VM's own keyboard buffer; nothing in Win32 can inject it. Not worth it.
2. Give the client a way to give up on one command without killing the
   session: a CANCEL record on the exec channel, mapped to the existing
   `wl_proc_kill`/reaper path. **Done 2026-09-22**: `WL_EXEC_CANCEL`, raised
   by Ctrl-C in `w98link exec`, answered immediately by the agent with EXIT
   130 and CLOSE. The wedged child goes to the reaper while the session
   stays up, so the next client is not waiting behind a teardown. Covered by
   `test_cancel_stops_the_command_on_the_agent`.
3. Keep the command away from DOS altogether. **Done 2026-09-22**, and this
   is what actually solved it: the agent refuses a command naming a drive
   letter with no media in it (`drives_are_ready` in `src/core/agent.c`,
   `wl_fs_drive_ready` per platform), because the 32-bit readiness call
   honours the error mode where `command.com` does not.

## Verified again on the target 2026-09-22, agent build 2206

```
$ w98link exec 'dir D:\'
w98link: drive D: is not ready                    (exit 125, 76 ms)
$ w98link info
agent    0.1.0_20260922.2206.win98                (43 ms)
$ w98link exec 'dir C:\W98AGENT'
 Volume in drive C is WIN98 ...                   (unaffected)
```

Closed. Two corrections to what is written above, both worth keeping:

- The claim that the agent "survives" a critical-error prompt was wrong. A
  later run left twelve clients unanswered over two minutes and the agent came
  back only when the DOS task was ended at the target. While that prompt is up
  the agent's process does not run at all, which also means CANCEL cannot
  reach it. The earlier apparent self-recovery must have been the prompt being
  dealt with on the machine.
- The prompt is written to the redirected pipe, not the screen, so there may
  be nothing visible to press F on. That it arrives over the pipe also says
  DOS is using the standard handles, so writing "F\r\n" to the child's stdin
  would probably answer it. v1 closes stdin at once (STDIN_EOF), so the VM
  gets end of file and waits forever. Not pursued, since the command is now
  refused before it starts, but it is the fallback if a program ever reaches
  empty media by a route the drive check cannot see.

Note for the manual: on Win9x anything that needs `command.com` (shell
builtins, redirection, pipes) runs in a DOS VM, so it neither reports an
exit code nor honours the error mode. Prefer real programs.
