@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"
for %%I in ("%SCRIPT_DIR%\..") do set "PROJECT_ROOT=%%~fI"
set "ZL="
set "STATUS=0"

if not "%~1"=="" (
    for %%I in ("%~1") do set "ZL=%%~fI"
) 
call :resolve_compiler
if not defined ZL (
    echo No ZL compiler was found. Building zl_language...
    call "%SCRIPT_DIR%\build.bat" --target zl_language --release
    if errorlevel 1 exit /b %ERRORLEVEL%
    call :resolve_compiler
)
if not defined ZL (
    echo error: zl_language.exe could not be located after the build. >&2
    exit /b 2
)
if not exist "%ZL%" (
    echo error: '%ZL%' does not exist. >&2
    exit /b 2
)

REM Examples live in topic subdirectories. Files under a `_lib` directory are
REM importable module sources with no main(), so skip them but hand each one to
REM the importer via --root.
set "EXAMPLES=%PROJECT_ROOT%\examples"
set "ROOTS="
for /f "delims=" %%D in ('dir /b /s /ad "%EXAMPLES%\_lib" 2^>nul') do set "ROOTS=!ROOTS! --root %%D"

set "FOUND=0"
for /f "delims=" %%F in ('dir /b /s /a-d "%EXAMPLES%\*.zl" 2^>nul ^| findstr /v /i "\\_lib\\" ^| sort') do (
    set "FOUND=1"
    echo --- %%~nxF ---
    "%ZL%" !ROOTS! "%%~fF" %*
    if errorlevel 1 set "STATUS=1"
    echo.
)
if "%FOUND%"=="0" (
    echo no examples found under %EXAMPLES% >&2
    exit /b 2
)
REM To also diff each example against its embedded expected output, run
REM examples\run_all.bat instead.
exit /b %STATUS%

:resolve_compiler
if defined ZL if exist "%ZL%" (
    for %%I in ("%ZL%") do set "ZL=%%~fI"
    exit /b 0
)
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
