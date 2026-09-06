@echo off
:: Double-click: builds, packages and RARs the current release into dist\.
:: Any extra arguments go to tools\make_rar.ps1 (e.g. -FromDist, -NoBuild).
setlocal
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\make_rar.ps1" %*
set RC=%ERRORLEVEL%
echo.
if %RC% neq 0 (
    echo make_rar FAILED with code %RC% - nothing usable was written.
) else (
    echo make_rar OK.
)
:: No arguments means a double-click: keep the window open so the result can be read.
if "%~1"=="" pause
endlocal & exit /b %RC%
