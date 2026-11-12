@echo off
setlocal
net session >nul 2>&1
if errorlevel 1 (
  echo Requesting administrator rights...
  powershell -NoProfile -Command "Start-Process -FilePath '%~f0' -Verb RunAs"
  exit /b
)
echo Adding Windows Firewall inbound rule for TCP 3414...
netsh advfirewall firewall delete rule name="GrinP2P" >nul 2>&1
netsh advfirewall firewall add rule name="GrinP2P" dir=in action=allow protocol=TCP localport=3414
if errorlevel 1 (
  echo FAILED to add the firewall rule.
) else (
  echo OK: inbound TCP 3414 is allowed.
)
echo.
echo Next: forward TCP 3414 on your router to this PC's local IP.
echo Check from another network: Test-NetConnection ^<public-ip^> -Port 3414
pause