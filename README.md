# devlink

A compressed file and command link between a Linux workstation and an old
Windows machine, for developing here and building and running there. It exists
because the options at that end each miss something: Kermit and Zmodem are
reliable but barely compress, and Windows' PPP compression (MPPC) is not
something Linux pppd speaks.

Three programs share one protocol core:

- `devlink`, the Linux client
- `DEVLINK.EXE`, the Win32 agent, cross-built with Open Watcom
- `devlink-agent`, the same agent for Linux, statically linked, for a rescue
  image that boots on a machine whose own OS will not (Manual section 5)

The target is **Windows 2000**. Windows 98 SE and NT 4.0 still work and the
code for them is kept, but nothing is verified on them any more; see
`docs/plan.md`. The names in this repo still say 98, from when it was the
target, and change when the project is named.

Two transports: a null-modem serial cable, and TCP, encrypted with a
pre-shared passphrase (Noise, ChaCha20-Poly1305). The agent serves both at
once, and any number of TCP clients; see `Manual.md`, "Over TCP".

- `Manual.md` is the operating manual: installing, running, tuning,
  diagnosing, and what the limits are.
- `docs/protocol.md` is the wire format.
- `docs/plan.md` is the iteration plan and what is done.

Measured over a 115200 serial cable to a Pentium III (a ThinkPad A21m, then
running Windows 98 SE): a 50-file source tree with nothing changed syncs in a
quarter of a second, and C source moves at about 35-40 KB/s against the line's
raw 11.5 KB/s.

Working name only; the project will be renamed.

## Building

Requirements: gcc, make, cmocka, gcovr, cppcheck, clang-tidy, and Open Watcom
v2 for the agent (unpacked at `~/.local/opt/watcom`, or set `WATCOM=`).

For reading the code, mingw-w64's headers as well: nothing is built with them,
but they are what lets clangd, an editor and `make tidy` parse `src/win32/`,
which no Linux compiler can do with windows.h absent. `compile_flags.txt` and
`.clangd` between them describe the whole tree, so every file should open
without errors. `make tidy` says so when it skips the Win32 half for want of
those headers.

```sh
make            # Linux client  -> build/bin/devlink
make agent      # Windows agent -> build/bin/DEVLINK.EXE (console)
                #                  build/bin/DEVLINKW.EXE (window)
make dist       # agents + launcher + README, staged for the target
                # (make dist DEST=/run/media/you/STICK writes it to a stick)
make test       # cmocka suite
make coverage   # line coverage, gated at 80%
make analyze    # cppcheck, gcc -fanalyzer, stack usage
make tidy       # clang-tidy, including the Win32 agent sources
make gui-preview  # photograph the windowed agent under Wine (no gate)
make help       # everything else
```

## License

GPL-3.0-or-later. See `LICENSE`.
