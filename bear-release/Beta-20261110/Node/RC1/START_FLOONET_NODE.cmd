@echo off
setlocal
title Grin Node RC1 - Floonet (node only)
echo Starting FLOONET node only.
echo Profile and logs are stored next to this launcher.
echo This is a test network; Mainnet is not started.
pushd "%~dp0"
if errorlevel 1 (
  echo Could not access the launcher directory.
  pause
  exit /b 1
)
set "NODE_HOME=%CD%\node-data"
for %%I in ("%NODE_HOME%") do (
  set "HOMEDRIVE=%%~dI"
  set "HOMEPATH=%%~pI%%~nxI"
)
if not exist "%NODE_HOME%" mkdir "%NODE_HOME%"
if errorlevel 1 (
  echo Could not create the node-data directory. Use a writable folder.
  popd
  pause
  exit /b 1
)
if not exist "%NODE_HOME%\temp" mkdir "%NODE_HOME%\temp"
if errorlevel 1 (
  echo Could not create the temporary directory in node-data.
  popd
  pause
  exit /b 1
)
set "TEMP=%NODE_HOME%\temp"
set "TMP=%NODE_HOME%\temp"
"GrinNode.exe" --floonet --no-wallet
set "NODE_EXIT=%ERRORLEVEL%"
echo.
echo Node exited with code %NODE_EXIT%.
if not "%NODE_EXIT%"=="0" (
  echo Check the output above and the logs directory.
  pause
)
popd
exit /b %NODE_EXIT%
