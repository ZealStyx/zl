@echo off
REM examples\run_all.bat - Windows twin of run_all.sh.
REM
REM Usage:
REM   examples\run_all.bat [path\to\zl_language.exe]
REM
REM Finds the runtime the same way (..\build\Release, ..\build, then PATH), runs
REM every example, and diffs each one's real stdout against the
REM "// Expected output" block at the bottom of the same file.
REM
REM The extraction and the comparison both happen in Python so that line endings
REM do not matter: zl_language writes LF, a redirected Windows console yields
REM CRLF, and a binary compare would fail on every example.
setlocal EnableExtensions EnableDelayedExpansion

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"
for %%I in ("%SCRIPT_DIR%\..") do set "ROOT=%%~fI"

set "ZL="
if not "%~1"=="" set "ZL=%~1"
if "%ZL%"=="" if exist "%ROOT%\build\Release\zl_language.exe" set "ZL=%ROOT%\build\Release\zl_language.exe"
if "%ZL%"=="" if exist "%ROOT%\build\zl_language.exe" set "ZL=%ROOT%\build\zl_language.exe"
if "%ZL%"=="" for /f "delims=" %%P in ('where zl_language 2^>nul') do if not defined ZL set "ZL=%%P"
if "%ZL%"=="" for /f "delims=" %%P in ('where zl 2^>nul') do if not defined ZL set "ZL=%%P"

if not defined ZL (
    echo error: cannot find the zl_language runtime 1>&2
    echo hint: build it first, or pass the path: run_all.bat path\to\zl_language.exe 1>&2
    exit /b 2
)
if not exist "%ZL%" (
    echo error: '%ZL%' does not exist 1>&2
    exit /b 2
)

where python >nul 2>nul
if errorlevel 1 (
    echo error: python is required to diff expected output 1>&2
    exit /b 2
)

REM Directories named `_lib` hold importable module sources (interfaces, data
REM records, helper classes with no main) rather than runnable examples. Skip
REM them, but hand each one to the importer through --root.
set "ROOTS="
for /f "delims=" %%D in ('dir /b /s /ad "%SCRIPT_DIR%\_lib" 2^>nul') do set "ROOTS=!ROOTS! --root %%D"

set "CHECK=%TEMP%\zl_check_%RANDOM%%RANDOM%.py"
call :writeHelper

set /a TOTAL=0
set /a PASSED=0
set /a SKIPPED=0
set "FAILED="

for /f "delims=" %%F in ('dir /b /s /a-d "%SCRIPT_DIR%\*.zl" 2^>nul ^| findstr /v /i "\\_lib\\" ^| sort') do (
    set "FILE=%%~fF"
    set "REL=%%~dpnxF"
    set "REL=!REL:%SCRIPT_DIR%\=!"
    set "ACTUAL=%TEMP%\zl_actual.txt"

    "%ZL%" !ROOTS! "!FILE!" > "!ACTUAL!" 2>&1
    set "RC=!ERRORLEVEL!"

    python "%CHECK%" "!FILE!" "!ACTUAL!" "!RC!"
    set "VERDICT=!ERRORLEVEL!"

    if "!VERDICT!"=="3" (
        echo SKIP  !REL!  ^(no Expected output block^)
        set /a SKIPPED+=1
    ) else if "!VERDICT!"=="0" (
        echo PASS  !REL!
        set /a PASSED+=1
        set /a TOTAL+=1
    ) else (
        echo FAIL  !REL!
        set "FAILED=!FAILED! !REL!"
        set /a TOTAL+=1
    )
)

del "%CHECK%" >nul 2>nul
del "%TEMP%\zl_actual.txt" >nul 2>nul

echo.
echo ------------------------------------------------------------
if not defined FAILED (
    echo !PASSED!/!TOTAL! PASS
    if !SKIPPED! GTR 0 echo ^(!SKIPPED! example^(s^) have no Expected output block and were skipped^)
    exit /b 0
)
echo !PASSED!/!TOTAL! PASS, some examples FAILED:
for %%X in (!FAILED!) do echo   - %%X
exit /b 1

REM ---------------------------------------------------------------------------
REM Helper: python zl_check.py <example.zl> <actual.txt> <exit-code>
REM   exit 0 = matched, 1 = mismatch or runtime failure, 3 = no expected block
REM ---------------------------------------------------------------------------
:writeHelper
> "%CHECK%" echo import sys
>>"%CHECK%" echo BEGIN = "// --- Expected output (verified by examples/run_all.sh) ---"
>>"%CHECK%" echo END = "// --- End expected output ---"
>>"%CHECK%" echo source, actual_path, code = sys.argv[1], sys.argv[2], sys.argv[3]
>>"%CHECK%" echo lines = open(source, encoding="utf-8").read().splitlines()
>>"%CHECK%" echo if BEGIN not in lines or END not in lines:
>>"%CHECK%" echo     sys.exit(3)
>>"%CHECK%" echo body = lines[lines.index(BEGIN) + 1:lines.index(END)]
>>"%CHECK%" echo expected = [l[3:] if l.startswith("// ") else ("" if l == "//" else l[2:]) for l in body]
>>"%CHECK%" echo raw = open(actual_path, "rb").read().decode("utf-8", "replace")
>>"%CHECK%" echo actual = raw.replace("\r\n", "\n").split("\n")
>>"%CHECK%" echo if actual and actual[-1] == "":
>>"%CHECK%" echo     actual.pop()
>>"%CHECK%" echo if code != "0":
>>"%CHECK%" echo     sys.stderr.write("        runtime exited " + code + "\n")
>>"%CHECK%" echo     for l in actual:
>>"%CHECK%" echo         sys.stderr.write("        ^| " + l + "\n")
>>"%CHECK%" echo     sys.exit(1)
>>"%CHECK%" echo if actual == expected:
>>"%CHECK%" echo     sys.exit(0)
>>"%CHECK%" echo import difflib
>>"%CHECK%" echo sys.stderr.write("        output differs from Expected output block\n")
>>"%CHECK%" echo for l in difflib.unified_diff(expected, actual, "expected", "actual", lineterm=""):
>>"%CHECK%" echo     sys.stderr.write("        " + l + "\n")
>>"%CHECK%" echo sys.exit(1)
exit /b 0
