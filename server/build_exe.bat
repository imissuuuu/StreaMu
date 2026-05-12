@echo off
cd /d %~dp0
echo === Building StreaMu-Server.exe ===
echo.

where py >nul 2>nul
if %errorlevel%==0 (
    set WINPY=py -3
) else (
    where python >nul 2>nul
    if %errorlevel%==0 (
        set WINPY=python
    ) else (
        echo [ERROR] Python not found. Install Python 3.10+ and add to PATH.
        pause
        exit /b 1
    )
)

if not exist venv_build\Scripts\python.exe (
    echo [INFO] Creating build venv...
    %WINPY% -m venv venv_build
)

call venv_build\Scripts\activate.bat
pip install -r requirements.txt pyinstaller
echo.
pyinstaller --onefile --name StreaMu-Server ^
    --collect-all yt_dlp ^
    --collect-all starlette ^
    --collect-all uvicorn ^
    --hidden-import anyio ^
    --hidden-import anyio._backends ^
    --hidden-import anyio._backends._asyncio ^
    proxy.py
echo.
if exist dist\StreaMu-Server.exe (
    echo [OK] Built: dist\StreaMu-Server.exe
) else (
    echo [ERROR] Build failed.
)
pause
