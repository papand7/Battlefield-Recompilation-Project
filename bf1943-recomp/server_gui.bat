@echo off
rem The dedicated server's window: the launcher, opened on its SERVER page (name, maps,
rem bots, internet, the bots' advisor, training runs; START / STOP, its log, the replay).
rem A server without a window: start_server.bat.
cd /d "%~dp0"
rem A release folder: Launcher\ next to this one (src\).
if exist "..\Launcher\BF1943Launcher.exe" (
  start "" "..\Launcher\BF1943Launcher.exe" --server
  exit /b
)
rem The single exe: BattlefieldLauncher-<version>.exe next to BattlefieldRecomp\ (the newest).
for /f "delims=" %%f in ('dir /b /o-d "..\..\BattlefieldLauncher-*.exe" 2^>nul') do (
  start "" "..\..\%%f" --server
  exit /b
)
rem From the sources: Electron (npm install in launcher\ first).
if exist "launcher\node_modules\electron\dist\electron.exe" (
  start "" "%~dp0launcher\node_modules\electron\dist\electron.exe" "%~dp0launcher" --server
  exit /b
)
echo The launcher was not found: run "npm install" in launcher\ (from the sources), or use
echo the launcher's SERVER page. start_server.bat runs a server without a window.
pause
