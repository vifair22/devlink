# Bug: a large, highly compressible file through the multiplexer arrives short

Status: fixed 2026-09-23. Reported 2026-09-23 against agent
`0.1.0_20260923.0648.agent` on Windows 2000 on the A21m, client
`0.1.0_20260923.1924.release`, over the serial line with `DEVLINK_MUX=1`.

## What happened

```
$ devlink get 'C:\W2KINST\SHOT.BMP' shot.bmp
devlink: received failed: checksum mismatch
```

A 393,334-byte bitmap, every time. The same command with the multiplexer out
of the way succeeded ten times out of ten, as did every other bitmap in that
directory, a 2.7 MB registry hive and a 5.6 MB log. What the failures had in
common was not the file but the environment: `DEVLINK_MUX=1`.

Six runs through the multiplexer, six failures, always the same count:

```
wrong number of bytes: expected 393334, received 389238
```

393334 - 389238 is 4096, one whole chunk. Nothing was corrupted; a record went
missing.

## Why

`up_record` in `src/posix/mux.c` hands each record from the line to the client
that asked for it, and keeps the record when the client's queue is full. It
kept exactly one, with this comment above it:

> One record that could not be handed on because the other side's queue was
> full. The source peer is not pumped again until it drains, so the sender's
> credit window does the throttling and nothing is ever dropped.

The loop does stop pumping a peer whose records are not being taken:
`if (!block_up) wl_peer_pump(m->up, line, 0)`. The flaw is that `up_record` is
a handler called from *inside* that pump, and one pump dispatches a whole
burst. The first record that will not fit is kept; every later one in the same
pump overwrites it. `block_up` can only stop the next pump, never the one
already running.

A counter on the overwrite confirmed it: zero before the transfer, one after.
One overwrite, one 4 KiB record, exactly the shortfall.

It needs the receiver to fall behind, which is why size and compressibility
together are what triggered it. A small file never fills the queue, and an
incompressible one arrives no faster than the line. A flat bitmap compresses
about fourteen to one, so the relay is handed fourteen bytes for every one the
cable carried.

`down_record` had the same hole, so a `put` through the multiplexer failed the
same way.

## The fix

The stash became a backlog: records in arrival order, in a buffer that grows
only when backpressure actually happens, and both directions drain as many as
the far side will take.

The cap is 64 KiB (`WL_SESSION_QUEUE`) per direction, and it is a bound rather
than a guess. `wl_peer_work` drains the session's plaintext queue and never
refills it part way through, so a single pump dispatches less than one queue,
and no further pump happens while anything is held. Reaching the cap would mean
that bound had stopped holding, which is a defect and not a busy line, so it
ends that channel with a line in the log instead of losing a record.

That last part is the point. The bug was not hard to find once it was visible;
it cost a day because it was silent. A dropped record now cannot be silent.

## What it says about the rest

Three defects in this file in three days, all the same shape: something was
handled on one side only. A close answered without sending ours. A deferred
close with a comment but no code to finish it. Now a stash whose invariant the
comment states correctly and the code does not enforce. The comment was right
about what had to be true; nothing checked that it was.

## Tests

`test_a_large_compressible_transfer_loses_nothing` in `tests/test_mux.c` puts
a 2 MB file of long runs through a real multiplexer to a real agent and gets it
back, comparing both. It fails against the code above.

Two more, from the same investigation, both in `tests/test_files.c`: the same
file straight down a pty, and again over a line that drops and flips bytes so
the link has to retransmit (`tests/noisy_line.h`). Both passed before the fix
and after it, which is what placed the fault in the multiplexer rather than in
the compression or the link.

## Also changed

Both ends reported a short file and a damaged one with the same words,
"checksum mismatch". They are now separate, and carry the numbers:

```
wrong number of bytes: expected 393334, received 389238
checksum mismatch over 393334 bytes: sent 1f8a3c0d, received 44b21e97
```

The first is the transport losing or repeating something; the second is the
bytes arriving wrong. They point at different code, and the first message is
what identified this one.
