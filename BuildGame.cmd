@echo off
rem SPDX-License-Identifier: GPL-3.0-or-later
setlocal DisableDelayedExpansion
if "%~1"=="" goto usage
if not "%~2"=="" goto usage
if exist "%~dp0tools\python\python.exe" goto bundled
py -3 -c "import sys; sys.exit(sys.version_info < (3, 12))" >nul 2>&1
if not errorlevel 1 goto py
python -c "import sys; sys.exit(sys.version_info < (3, 12))" >nul 2>&1
if not errorlevel 1 goto python
echo Install Python 3.12 or newer, then see docs/building.md for the build tools.
pause
exit /b 1
:bundled
set "PYTHONHOME=%~dp0tools\python"
set "PYTHONPATH="
set "PYTHONNOUSERSITE=1"
"%~dp0tools\python\python.exe" "%~dp0tools\build_game.py" --install-prerequisites --iso "%~f1"
goto done
:py
py -3 "%~dp0tools\build_game.py" --install-prerequisites --iso "%~f1"
goto done
:python
python "%~dp0tools\build_game.py" --install-prerequisites --iso "%~f1"
:done
set "result=%errorlevel%"
echo.
if "%result%"=="0" echo Build complete. Open Launcher.exe to choose settings, or DOAXBV.exe to play with the saved ones.
pause
exit /b %result%
:usage
echo Drag one Xbox ISO onto BuildGame.cmd.
echo Setup downloads the llvm-mingw compiler into tools\llvm-mingw on first use.
pause
exit /b 1
