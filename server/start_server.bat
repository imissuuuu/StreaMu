@echo off
cd /d %~dp0
title StreaMu - Proxy Server Console
echo ==============================================
echo  StreaMu - Proxy Server (Starlette)
echo ==============================================
echo.
echo Starting the server...
echo (You can close this window to stop the server at any time)
echo.

if exist venv\Scripts\python.exe (
    venv\Scripts\python.exe proxy.py
) else (
    venv\bin\python.exe proxy.py
)

echo.
pause
