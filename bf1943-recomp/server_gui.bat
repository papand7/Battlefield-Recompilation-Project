@echo off
rem The dedicated server's window (tools\server_gui.py): name, maps, bots, internet,
rem the bots' advisor, training runs; START / STOP, its log, the training replay.
cd /d "%~dp0"
where pythonw >nul 2>&1 && (start "" pythonw tools\server_gui.py & exit /b)
where pyw >nul 2>&1 && (start "" pyw -3 tools\server_gui.py & exit /b)
echo Python is needed (the launcher installs it when it sets up the game): https://www.python.org/
pause
