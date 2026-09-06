@echo off
setlocal EnableExtensions EnableDelayedExpansion

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"
for %%I in ("%SCRIPT_DIR%\..") do set "PROJECT_ROOT=%%~fI"
set "VALID_ROOT=%PROJECT_ROOT%\tests\zl\valid"
set "INVALID_ROOT=%PROJECT_ROOT%\tests\zl\invalid"
set "ZL="
set "ZLPKG="
set "MODE_ARG=%~1"
set "SELECTION_ARG=%~2"
set "PASS_COUNT=0"
set "FAIL_COUNT=0"
set "SKIP_COUNT=0"
set "PARSE_PASS=0"
set "PARSE_FAIL=0"
set "RUNNER_VERSION=2026-09-06-stable-corpus-v11"

if /i "%MODE_ARG%"=="--help" goto :usage
if /i "%MODE_ARG%"=="-h" goto :usage
if /i "%MODE_ARG%"=="--check-all" goto :check_all
if /i "%MODE_ARG%"=="--list" goto :list_all
if /i "%MODE_ARG%"=="-l" goto :list_all

call :resolve_compiler
if not defined ZL (
    echo No ZL compiler was found. Building zl_language...
    call "%SCRIPT_DIR%\build.bat" --target zl_language --release
    if errorlevel 1 exit /b %ERRORLEVEL%
    call :resolve_compiler
)
if not defined ZL (
    echo error: zl_language.exe could not be located. 1>&2
    exit /b 2
)
call :resolve_zlpkg

call :normalize_mode "%MODE_ARG%"
if not defined SCOPE (
    echo 1. Test All
    echo 2. Invalid
    echo 3. Valid
    set /p "CHOICE=Select mode: "
    call :normalize_mode "!CHOICE!"
)
if not defined SCOPE (
    echo error: unrecognized selection. 1>&2
    exit /b 2
)

set /a IDX=0
if /i "%SCOPE%"=="all" (
    call :print_section "Invalid" "%INVALID_ROOT%" 1
    call :print_section "Valid" "%VALID_ROOT%" 0
) else if /i "%SCOPE%"=="invalid" (
    call :print_section "Invalid" "%INVALID_ROOT%" 1
) else (
    call :print_section "Valid" "%VALID_ROOT%" 0
)
if "%IDX%"=="0" (
    echo error: no test categories found. 1>&2
    exit /b 2
)
if "%SELECTION_ARG%"=="" set "SELECTION_ARG=0"
if "%SELECTION_ARG%"=="0" (
    for /l %%N in (1,1,%IDX%) do call :run_category %%N
) else (
    for %%N in (%SELECTION_ARG:,= %) do call :run_category %%N
)

echo.
echo %PASS_COUNT% passed, %FAIL_COUNT% failed, %SKIP_COUNT% skipped
if not "%FAIL_COUNT%"=="0" exit /b 1
exit /b 0

:check_all
call :resolve_compiler
if not defined ZL (
    echo No ZL compiler was found. Building zl_language...
    call "%SCRIPT_DIR%\build.bat" --target zl_language --release
    if errorlevel 1 exit /b %ERRORLEVEL%
    call :resolve_compiler
)
if not defined ZL (
    echo error: zl_language.exe could not be located. 1>&2
    exit /b 2
)
call :resolve_zlpkg

echo === Parse sweep: all ZL files ===
call :parse_tree "%VALID_ROOT%" 0
call :parse_tree "%INVALID_ROOT%" 1
call :parse_tree "%PROJECT_ROOT%\stdlib" 0
call :parse_tree "%PROJECT_ROOT%\examples" 0

if not "%PARSE_FAIL%"=="0" (
    echo.
echo Parse sweep failures: %PARSE_FAIL%
    exit /b 1
)

echo.
echo === Semantic/runtime corpus ===
set "SCOPE=all"
set /a IDX=0
call :print_section "Invalid" "%INVALID_ROOT%" 1
call :print_section "Valid" "%VALID_ROOT%" 0
if "%IDX%"=="0" exit /b 2
for /l %%N in (1,1,%IDX%) do call :run_category %%N

echo.
echo === Corpus summary ===
echo %PARSE_PASS% files parsed successfully; %PARSE_FAIL% unexpected parse failures.
echo %PASS_COUNT% executable/package cases passed, %FAIL_COUNT% failed, %SKIP_COUNT% skipped.
if not "%FAIL_COUNT%"=="0" exit /b 1
if not "%SKIP_COUNT%"=="0" exit /b 1
exit /b 0

:parse_tree
set "PARSE_ROOT=%~1"
set "PARSE_EXPECT_INVALID=%~2"
if not exist "%PARSE_ROOT%" exit /b 0
for /r "%PARSE_ROOT%" %%F in (*.zl) do call :parse_one "%%~fF" "%PARSE_EXPECT_INVALID%"
exit /b 0

:parse_one
set "PARSE_FILE=%~1"
set "PARSE_EXPECT_INVALID=%~2"
set "OUT_FILE=%TEMP%\zl_parse_%RANDOM%%RANDOM%.out"
set "ERR_FILE=%TEMP%\zl_parse_%RANDOM%%RANDOM%.err"
"%ZL%" --parse-only "%PARSE_FILE%" >"%OUT_FILE%" 2>"%ERR_FILE%"
set "RC=%ERRORLEVEL%"
if "%PARSE_EXPECT_INVALID%"=="0" (
    if "%RC%"=="0" (set /a PARSE_PASS+=1) else (
        echo   FAIL parse  %PARSE_FILE%
        if exist "%ERR_FILE%" type "%ERR_FILE%"
        set /a PARSE_FAIL+=1
    )
) else (
    if "%RC%"=="0" (set /a PARSE_PASS+=1) else if "%RC%"=="1" (set /a PARSE_PASS+=1) else (
        echo   FAIL parser-crash  %PARSE_FILE%
        if exist "%ERR_FILE%" type "%ERR_FILE%"
        set /a PARSE_FAIL+=1
    )
)
del /q "%OUT_FILE%" "%ERR_FILE%" >nul 2>nul
exit /b 0

:run_category
set "N=%~1"
set "ROOT=!CAT_ROOT_%N%!"
set "NAME=!CAT_NAME_%N%!"
set "EXPECTED=!CAT_EXPECTED_%N%!"
if not defined NAME exit /b 0
echo.
if "%EXPECTED%"=="0" (echo --- %NAME% ^(valid^) ---) else (echo --- %NAME% ^(invalid^) ---)
for %%F in ("%ROOT%\%NAME%\*.zl") do if exist "%%~fF" (
    findstr /c:"func main(" "%%~fF" >nul 2>nul && call :run_case "%%~nF" "zl" "%%~fF" "%EXPECTED%"
)
for /d %%D in ("%ROOT%\%NAME%\*") do (
    call :classify_case "%%~fD"
    if defined CASE_KIND call :run_case "%%~nxD" "!CASE_KIND!" "!CASE_LOCATION!" "%EXPECTED%"
)
exit /b 0

:run_case
set "NAME=%~1"
set "KIND=%~2"
set "LOCATION=%~3"
set "EXPECTED=%~4"
if /i "%KIND%"=="pkg" goto :run_pkg_case
set "OUT_FILE=%TEMP%\zl_test_%RANDOM%%RANDOM%.out"
set "ERR_FILE=%TEMP%\zl_test_%RANDOM%%RANDOM%.err"
"%ZL%" "%LOCATION%" >"%OUT_FILE%" 2>"%ERR_FILE%"
set "ACTUAL=%ERRORLEVEL%"
set "TERM_MARKER=%LOCATION%.expect-termination"
if exist "%TERM_MARKER%" (
    if not "%ACTUAL%"=="0" (echo   PASS %NAME% ^(expected termination^)&set /a PASS_COUNT+=1) else (echo   FAIL %NAME% ^(expected termination but exited 0^)&set /a FAIL_COUNT+=1)
) else if "%ACTUAL%"=="%EXPECTED%" (echo   PASS %NAME%&set /a PASS_COUNT+=1) else (echo   FAIL %NAME%&type "%OUT_FILE%" 2>nul&type "%ERR_FILE%" 2>nul&set /a FAIL_COUNT+=1)
del /q "%OUT_FILE%" "%ERR_FILE%" >nul 2>nul
exit /b 0

:run_pkg_case
if not defined ZLPKG (echo   FAIL %NAME% ^(zlpkg.exe not found^)&set /a FAIL_COUNT+=1&exit /b 0)
call :find_entry "%LOCATION%"
if not defined FOUND_ENTRY (echo   FAIL %NAME% ^(no main entry^)&set /a FAIL_COUNT+=1&exit /b 0)
set "REL_ENTRY=%FOUND_ENTRY:%LOCATION%\=%"
if exist "%LOCATION%\.zlpkg" rmdir /s /q "%LOCATION%\.zlpkg" >nul 2>nul
if exist "%LOCATION%\zlpkg.lock" del /q "%LOCATION%\zlpkg.lock" >nul 2>nul
set "OUT_FILE=%TEMP%\zlpkg_%RANDOM%%RANDOM%.out"
set "ERR_FILE=%TEMP%\zlpkg_%RANDOM%%RANDOM%.err"
pushd "%LOCATION%"
"%ZLPKG%" run "%REL_ENTRY%" >"%OUT_FILE%" 2>"%ERR_FILE%"
set "ACTUAL=%ERRORLEVEL%"
popd
if exist "%LOCATION%\.zlpkg" rmdir /s /q "%LOCATION%\.zlpkg" >nul 2>nul
if exist "%LOCATION%\zlpkg.lock" del /q "%LOCATION%\zlpkg.lock" >nul 2>nul
if "%ACTUAL%"=="%EXPECTED%" (echo   PASS %NAME%&set /a PASS_COUNT+=1) else (echo   FAIL %NAME%&type "%OUT_FILE%" 2>nul&type "%ERR_FILE%" 2>nul&set /a FAIL_COUNT+=1)
del /q "%OUT_FILE%" "%ERR_FILE%" >nul 2>nul
exit /b 0

:resolve_compiler
set "ZL="
if defined ZL_COMPILER_PATH if exist "%ZL_COMPILER_PATH%" (for %%I in ("%ZL_COMPILER_PATH%") do set "ZL=%%~fI"&exit /b 0)
for %%P in ("%PROJECT_ROOT%\build\Release\zl_language.exe" "%PROJECT_ROOT%\build\Debug\zl_language.exe" "%PROJECT_ROOT%\build\zl_language.exe" "%PROJECT_ROOT%\zl_language.exe") do if not defined ZL if exist "%%~P" set "ZL=%%~fP"
exit /b 0

:resolve_zlpkg
set "ZLPKG="
if defined ZLPKG_PATH if exist "%ZLPKG_PATH%" (for %%I in ("%ZLPKG_PATH%") do set "ZLPKG=%%~fI"&exit /b 0)
for %%P in ("%PROJECT_ROOT%\build\Release\zlpkg.exe" "%PROJECT_ROOT%\build\Debug\zlpkg.exe" "%PROJECT_ROOT%\build\zlpkg.exe" "%PROJECT_ROOT%\zlpkg.exe") do if not defined ZLPKG if exist "%%~P" set "ZLPKG=%%~fP"
exit /b 0

:print_section
set "LABEL=%~1"
set "ROOT=%~2"
set "EXPECTED=%~3"
if not exist "%ROOT%" exit /b 0
echo.
echo === %LABEL% ===
for /d %%C in ("%ROOT%\*") do (
    set /a IDX+=1
    set "CAT_ROOT_!IDX!=%ROOT%"
    set "CAT_NAME_!IDX!=%%~nxC"
    set "CAT_EXPECTED_!IDX!=%EXPECTED%"
    call :count_cases "%%~fC" CNT
    echo !IDX!. %%~nxC  ^(!CNT! cases^)
)
exit /b 0

:count_cases
set /a CNT=0
for %%F in ("%~1\*.zl") do if exist "%%~fF" findstr /c:"func main(" "%%~fF" >nul 2>nul && set /a CNT+=1
for /d %%D in ("%~1\*") do (call :classify_case "%%~fD"&if defined CASE_KIND set /a CNT+=1)
set "%~2=%CNT%"
exit /b 0

:classify_case
set "CASE_KIND="
set "CASE_LOCATION="
if exist "%~1\zlpkg.toml" (set "CASE_KIND=pkg"&for %%I in ("%~1") do set "CASE_LOCATION=%%~fI"&exit /b 0)
if exist "%~1\project\zlpkg.toml" (set "CASE_KIND=pkg"&for %%I in ("%~1\project") do set "CASE_LOCATION=%%~fI"&exit /b 0)
call :find_entry "%~1"
if defined FOUND_ENTRY (set "CASE_KIND=zl"&set "CASE_LOCATION=%FOUND_ENTRY%")
exit /b 0

:find_entry
set "FOUND_ENTRY="
REM Prefer an explicit Main.zl at the case/project root, then any root-level
REM .zl entry, then a recursive Main.zl, and finally any recursive entry.
REM This prevents helper modules with their own main() from hijacking a case.
if exist "%~1\Main.zl" (
    findstr /c:"func main(" "%~1\Main.zl" >nul 2>nul && set "FOUND_ENTRY=%~1\Main.zl"
)
if not defined FOUND_ENTRY for %%F in ("%~1\*.zl") do if not defined FOUND_ENTRY (
    findstr /c:"func main(" "%%~fF" >nul 2>nul && set "FOUND_ENTRY=%%~fF"
)
if not defined FOUND_ENTRY for /r "%~1" %%F in (Main.zl) do if not defined FOUND_ENTRY (
    findstr /c:"func main(" "%%~fF" >nul 2>nul && set "FOUND_ENTRY=%%~fF"
)
if not defined FOUND_ENTRY for /r "%~1" %%F in (*.zl) do if not defined FOUND_ENTRY (
    findstr /c:"func main(" "%%~fF" >nul 2>nul && set "FOUND_ENTRY=%%~fF"
)
exit /b 0

:list_all
call :resolve_compiler
if not defined ZL echo compiler not found
echo ZL regression runner %RUNNER_VERSION%
for /d %%D in ("%VALID_ROOT%\*") do echo VALID %%~nxD
for /d %%D in ("%INVALID_ROOT%\*") do echo INVALID %%~nxD
exit /b 0

:usage
echo Usage: scripts\run_regressions.bat [all^|valid^|invalid] [selection]
echo        scripts\run_regressions.bat --check-all
echo        scripts\run_regressions.bat --list
exit /b 0

:normalize_mode
set "SCOPE="
if /i "%~1"=="1" set "SCOPE=all"
if /i "%~1"=="all" set "SCOPE=all"
if /i "%~1"=="2" set "SCOPE=invalid"
if /i "%~1"=="invalid" set "SCOPE=invalid"
if /i "%~1"=="3" set "SCOPE=valid"
if /i "%~1"=="valid" set "SCOPE=valid"
exit /b 0
