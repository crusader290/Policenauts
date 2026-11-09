@echo off
rem Build Policenauts: fetch what's missing, then extract, recompile and compile.
rem
rem   build.bat                        uses the disc images in rom\ (Disc 1 required)
rem   build.bat Disc1.bin [Disc2.bin]  uses these images instead
rem
rem Needs Python 3.11+ and a gcc + ninja toolchain. Anything missing is fetched:
rem   Python  - installed per-user with winget
rem   ninja   - pip package in .venv\ (with the other Python requirements)
rem   gcc     - gcc on PATH, else w64devkit in tools\toolchain\, else MSYS2 ucrt64,
rem             else w64devkit is downloaded to tools\toolchain\
setlocal EnableExtensions
cd /d "%~dp0"

set "W64DEVKIT_VER=2.10.1"
set "W64DEVKIT_URL=https://github.com/skeeto/w64devkit/releases/download/v%W64DEVKIT_VER%/w64devkit-x64-%W64DEVKIT_VER%.7z.exe"
set "TOOLCHAIN=%~dp0tools\toolchain"

rem ---- Python ------------------------------------------------------------------------
if exist ".venv\Scripts\python.exe" goto have_venv
set "PY="
call :try_python py -3
if not defined PY call :try_python python
if not defined PY call :try_python "%LOCALAPPDATA%\Programs\Python\Launcher\py.exe" -3
if defined PY goto make_venv
echo [build] Python 3.11+ not found, installing it with winget...
where winget >nul 2>&1 || goto no_python
winget install -e --id Python.Python.3.12 --scope user --accept-package-agreements --accept-source-agreements
call :try_python "%LOCALAPPDATA%\Programs\Python\Python312\python.exe"
if not defined PY goto no_python
:make_venv
echo [build] creating .venv with %PY%
%PY% -m venv .venv || goto fail
:have_venv
set "VPY=%~dp0.venv\Scripts\python.exe"
"%VPY%" -m pip install --disable-pip-version-check -q -r requirements.txt || goto fail
set "PATH=%~dp0.venv\Scripts;%PATH%"

rem ---- gcc ---------------------------------------------------------------------------
where gcc >nul 2>&1 && goto have_gcc
if exist "%TOOLCHAIN%\w64devkit\bin\gcc.exe" goto add_devkit
if exist "C:\msys64\ucrt64\bin\gcc.exe" (
    set "PATH=C:\msys64\ucrt64\bin;%PATH%"
    goto have_gcc
)
echo [build] gcc not found, downloading w64devkit %W64DEVKIT_VER% (about 65 MB)...
if not exist "%TOOLCHAIN%" mkdir "%TOOLCHAIN%"
curl.exe -fL --retry 3 -o "%TOOLCHAIN%\w64devkit.7z.exe" "%W64DEVKIT_URL%" || goto fail
rem self-extracting 7-Zip archive: unpacks to %TOOLCHAIN%\w64devkit\
"%TOOLCHAIN%\w64devkit.7z.exe" -y -o"%TOOLCHAIN%" >nul || goto fail
del "%TOOLCHAIN%\w64devkit.7z.exe"
if not exist "%TOOLCHAIN%\w64devkit\bin\gcc.exe" (
    echo [build] w64devkit did not unpack where expected
    goto fail
)
:add_devkit
set "PATH=%TOOLCHAIN%\w64devkit\bin;%PATH%"
:have_gcc
for /f "delims=" %%G in ('where gcc') do (
    echo [build] using %%G
    goto gcc_shown
)
:gcc_shown

rem ---- configure, extract + recompile, configure again (now with the sources), build --
set CONFIGURE_ARGS=
if not "%~1"=="" set CONFIGURE_ARGS=--disc "%~1"
if not "%~2"=="" set CONFIGURE_ARGS=%CONFIGURE_ARGS% --disc2 "%~2"
"%VPY%" configure.py %CONFIGURE_ARGS% || goto fail
ninja generated/funcs.h || goto fail
"%VPY%" configure.py %CONFIGURE_ARGS% || goto fail
ninja || goto fail

echo.
echo [build] done: build\policenauts.exe
exit /b 0

rem ---- helpers -----------------------------------------------------------------------
:try_python
%* -c "import sys; sys.exit(sys.version_info < (3, 11))" >nul 2>&1 && set PY=%*
exit /b 0

:no_python
echo [build] Python 3.11 or newer is required: https://www.python.org/downloads/
:fail
echo.
echo [build] FAILED
exit /b 1
