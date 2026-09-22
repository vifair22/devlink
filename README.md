# w98link

A compressed file and command link between a Linux workstation and a Windows 98
machine. It exists because the options available on Windows 98 each miss
something: Kermit and Zmodem are reliable but barely compress, and Win98's
PPP compression (MPPC) is not something Linux pppd speaks.

Two programs share one protocol core:

- `w98link`, the Linux client
- `W98AGENT.EXE`, the Win32 agent, cross-built with Open Watcom; it targets
  Windows 98 SE and NT 4.0 or later

The first transport is a null-modem serial cable; TCP follows once the target
has a working network card.

- `Manual.md` is the operating manual: installing, running, tuning,
  diagnosing, and what the limits are.
- `docs/protocol.md` is the wire format.
- `docs/plan.md` is the iteration plan and what is done.

Over a 115200 serial cable to a Pentium III running Windows 98 SE, a
50-file source tree with nothing changed syncs in a quarter of a second, and
C source moves at about 35-40 KB/s against the line's raw 11.5 KB/s.

Working name only; the project will be renamed.

## Building

Requirements: gcc, make, cmocka, gcovr, cppcheck, clang-tidy, and Open Watcom
v2 for the agent (unpacked at `~/.local/opt/watcom`, or set `WATCOM=`).

```sh
make            # Linux client -> build/bin/w98link
make win98      # Win98 agent  -> build/bin/W98AGENT.EXE
make test       # cmocka suite
make coverage   # line coverage, gated at 80%
make analyze    # cppcheck, gcc -fanalyzer, stack usage
make help       # everything else
```

## License

GPL-3.0-or-later. See `LICENSE`.
