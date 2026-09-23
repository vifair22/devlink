devlink agent for Windows 2000
==============================

Built 0.1.0_20260922.2226.release. This is the target half of the link: the workstation
runs the client over a null-modem cable on COM1 and this serves it.


Installing
----------

1. Copy this whole DEVLINK folder to the root of C:, so that
   C:\DEVLINK\DEVLINK.EXE and C:\DEVLINK\AGENT.BAT exist.
   The path matters: AGENT.BAT changes into C:\DEVLINK.

2. Nothing else. There is no installer, no registry entry, no service, and
   no DLL to place. The agent is one executable using only KERNEL32 and
   USER32, and it writes nothing outside the directories a client names.


In a window instead
-------------------

DEVLINKW.EXE is the same agent with a window. Run it and it starts serving
at once, on COM1 at 115200 unless told otherwise (DEVLINKW COM2 115200).

Minimising it puts it in the notification area by the clock, where the
tooltip says whether a client is connected and a right-click gives Open,
Start, Stop and Exit. "DEVLINKW -tray" starts it there, which is what a
shortcut in Startup wants.

Right-clicking the log gives Copy line and Copy all, which beats reading an
error off this screen and typing it in somewhere else.

The window shows what the agent is doing now, which the console cannot: one
line per command or transfer in progress, with bytes moved and elapsed time,
since one client can run several at once. Below that is the same log the
console prints, and along the bottom, who is connected and the totals.

Stop releases the port; Start takes it again. Both take a moment, because the
agent finishes what it is doing and shuts its commands down properly rather
than being cut off. Closing the window does the same.

Use whichever suits. The console one is what AGENT.BAT runs and what the
diagnostics below need; the windowed one is easier to leave running and to
glance at.


Running
-------

Run AGENT.BAT. It prints one line and then waits:

    devlink agent 0.1.0_20260922.2226.release serving on COM1 at 115200

Leave that window open. The agent serves one client at a time, for as long
as it runs; each new client replaces the previous one. Everything else you
see in the window is a log line: a client connecting, a command being run, a
file being written, an error.

To stop it: Ctrl-C, then Y to "Terminate batch job".

If it will not start, the usual cause is that another program holds COM1, or
the serial port is not COM1 on this machine. Check Device Manager under
Ports (COM & LPT); if the port is different, edit the line in AGENT.BAT that
reads "DEVLINK.EXE serve COM1 115200".


Updating it
-----------

Do not copy a new binary over by hand. The workstation does this:

    devlink put build/bin/DEVLINK.EXE C:/DEVLINK/DEVLINK.NEW --force
    devlink exec @stop

The windowed agent updates the same way, under its own name:

    devlink put build/bin/DEVLINKW.EXE C:/DEVLINK/DEVLINKW.NEW --force
    devlink exec @stop

It has no launcher behind it and needs none: on @stop it renames itself to
DEVLINKW.OLD, moves the upload into place and starts it. The command line
carries over, so an agent running with -tray stays in the tray afterwards.
DEVLINKW.OLD is the build it replaced, kept as the way back, and normally
tidied up a few seconds later.

The agent exits, AGENT.BAT moves DEVLINK.NEW into place, and serving
resumes within a few seconds. Confirm with "devlink info", which prints the
build the agent is actually running.


Checking the cable without the client
-------------------------------------

    DEVLINK.EXE echo COM1 115200        byte echo, for a suspect cable
    DEVLINK.EXE linktest COM1 115200    the reliable link, with statistics
    DEVLINK.EXE --version

These are diagnostics only; the workstation has matching commands.


Notes for this machine
----------------------

The A21m has one 16550A at 3F8/IRQ4, which is COM1, and 115200 is as fast as
it goes. That is not a driver limit to be worked around: the chip divides a
1.8432 MHz clock by 16 and then by an integer, and 115200 is what a divisor
of 1 gives. There is no half a divisor, so the next rate up does not exist.

TCP is not built yet. When the Intel PRO/100 card arrives the same agent
will gain a network mode; until then the cable is the only route.
