@echo off
setlocal EnableExtensions

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"
for %%I in ("%SCRIPT_DIR%\..") do set "PROJECT_ROOT=%%~fI"

call :resolve_compiler
if not defined ZL (
    echo error: zl_language.exe was not found. >&2
    echo        Expected one of: build\Release\zl_language.exe, build\Debug\zl_language.exe, build\zl_language.exe, or zl_language.exe. >&2
    echo        Run scripts\build.bat first. >&2
    exit /b 2
)

"%ZL%" %*
exit /b %ERRORLEVEL%

:resolve_compiler
set "ZL="
if defined ZL_COMPILER_PATH if exist "%ZL_COMPILER_PATH%" (
    for %%I in ("%ZL_COMPILER_PATH%") do set "ZL=%%~fI"
    exit /b 0
)

for %%P in (
    "%PROJECT_ROOT%\build\Release\zl_language.exe"
    "%PROJECT_ROOT%\build\Debug\zl_language.exe"
    "%PROJECT_ROOT%\build\zl_language.exe"
    "%PROJECT_ROOT%\zl_language.exe"
) do (
    if not defined ZL if exist "%%~P" set "ZL=%%~fP"
)
exit /b 0
