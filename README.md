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
has a working network card. See `docs/plan.md` for the iteration plan.

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
