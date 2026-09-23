@echo off
rem The w98link agent on Windows 2000.
rem
rem Starting it: run this file. Leave the window open; it serves one client
rem at a time for as long as it runs.
rem
rem Updating it: the client uploads W98AGENT.NEW and runs "@stop". The agent
rem exits 0, this loop moves the new binary into place, and serving resumes.
rem
rem Stopping it: Ctrl-C, then Y to the "Terminate batch job" prompt.
cd /d C:\W98AGENT
:top
W98AGENT.EXE serve COM1 115200
if errorlevel 1 goto failed
if exist W98AGENT.NEW move /y W98AGENT.NEW W98AGENT.EXE >nul
goto top
:failed
echo.
echo The agent could not run. Usual cause: another program holds COM1, or
echo the port is not COM1 on this machine (check Device Manager, Ports).
echo Fix that and run AGENT.BAT again.
pause
