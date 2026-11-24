@echo off
rem Starts PRISM in Windows Terminal + WSL with the mouse-lock helper (mouse look in first person).
rem Extra arguments are passed to prism, e.g.  tools\Prism.bat --level 3
setlocal
set "TITLE=Prism"
set "ROOT=%~dp0.."
for %%I in ("%ROOT%") do set "ROOT=%%~fI"

if exist "%ROOT%\tools\mouselock.exe" (
    start "" /b "%ROOT%\tools\mouselock.exe" --title "%TITLE%"
) else (
    echo [info] tools\mouselock.exe not found - keyboard look only. Build it in WSL with: make winhelper
)

rem NOTE: no semicolons in the bash command (wt treats them as separators).
where wt >nul 2>nul
if %errorlevel%==0 (
    wt -w new --title "%TITLE%" --suppressApplicationTitle wsl.exe --cd "%ROOT%" -- bash -lc "[ -x ./prism ] || make && ./prism %*"
) else (
    title %TITLE%
    wsl.exe --cd "%ROOT%" -- bash -lc "[ -x ./prism ] || make && ./prism %*"
)

rem Restore the pointer in case the helper is still running after the game exits.
if exist "%ROOT%\tools\mouselock.exe" "%ROOT%\tools\mouselock.exe" --restore
endlocal
