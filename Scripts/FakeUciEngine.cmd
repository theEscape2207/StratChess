@echo off
REM Launcher for FakeUciEngine.ps1. Invoke-UciSearchToBestMove starts its ExePath with
REM UseShellExecute=false and a fixed 'uci' argument, and .NET refuses a .ps1 as an
REM executable; a .cmd is accepted and ignores the argument. Nothing else lives here.
pwsh -NoProfile -ExecutionPolicy Bypass -File "%~dp0FakeUciEngine.ps1"
exit /b %ERRORLEVEL%
