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

The A21m has one 16550A at 3F8/IRQ4, which is COM1. 115200 is what the
cable and the 98 driver would carry; Windows 2000's driver may go faster,
and that is worth trying once the link is up, because it is free throughput.

TCP is not built yet. When the Intel PRO/100 card arrives the same agent
will gain a network mode; until then the cable is the only route.
