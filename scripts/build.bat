@echo off
setlocal EnableExtensions EnableDelayedExpansion

rem ---------------------------------------------------------------------------
rem scripts\build.bat - ZL build driver.
rem
rem The default build is the CORE developer build: the ZL executable, the
rem package manager, the binding generator, and every compiler/runtime source
rem they depend on. Tests, benchmarks and examples are NOT in that graph; they
rem are reached through explicit modes.
rem
rem   build.bat          core toolchain     (target: zl-core)
rem   build.bat test     core + all tests   (target: zl-tests)
rem   build.bat full     everything         (target: zl-full)
rem   build.bat clean    remove build dir
rem   build.bat help     show this help
rem ---------------------------------------------------------------------------

set "SCRIPT_DIR=%~dp0"
if "%SCRIPT_DIR:~-1%"=="\" set "SCRIPT_DIR=%SCRIPT_DIR:~0,-1%"

for %%I in ("%SCRIPT_DIR%\..") do set "SOURCE_DIR=%%~fI"
set "BUILD_DIR=%SOURCE_DIR%\build"
set "CONFIG=Release"
set "GENERATOR="
set "TARGET="
set "JOBS=4"
set "DO_CONFIGURE=1"
set "DO_BUILD=1"
set "DO_CLEAN=0"
set "DO_RUN=0"
set "DO_CTEST=0"
set "MODE=core"
set "RUN_FILE=%SOURCE_DIR%\examples\Hello.zl"
set "EXTRA_CMAKE_ARGS="

:parse_args
if "%~1"=="" goto :main

if /i "%~1"=="-h" goto :help
if /i "%~1"=="--help" goto :help
if /i "%~1"=="help" goto :help

rem --- Build modes (positional, project-level aggregate targets) -------------
if /i "%~1"=="core" (
	set "MODE=core"
	shift
	goto :parse_args
)
if /i "%~1"=="test" (
	set "MODE=test"
	shift
	goto :parse_args
)
if /i "%~1"=="tests" (
	set "MODE=test"
	shift
	goto :parse_args
)
if /i "%~1"=="full" (
	set "MODE=full"
	shift
	goto :parse_args
)
if /i "%~1"=="all" (
	set "MODE=full"
	shift
	goto :parse_args
)
if /i "%~1"=="clean" (
	set "MODE=clean"
	shift
	goto :parse_args
)

if /i "%~1"=="--debug" (
	set "CONFIG=Debug"
	shift
	goto :parse_args
)
if /i "%~1"=="--release" (
	set "CONFIG=Release"
	shift
	goto :parse_args
)
if /i "%~1"=="--clean" (
	set "DO_CLEAN=1"
	shift
	goto :parse_args
)
if /i "%~1"=="--ctest" (
	set "DO_CTEST=1"
	set "MODE=test"
	shift
	goto :parse_args
)
if /i "%~1"=="--reconfigure" (
	set "DO_CONFIGURE=1"
	shift
	goto :parse_args
)
if /i "%~1"=="--no-configure" (
	set "DO_CONFIGURE=0"
	shift
	goto :parse_args
)
if /i "%~1"=="--build-only" (
	set "DO_CONFIGURE=0"
	set "DO_BUILD=1"
	shift
	goto :parse_args
)
if /i "%~1"=="--configure-only" (
	set "DO_BUILD=0"
	shift
	goto :parse_args
)
if /i "%~1"=="--run" (
	set "DO_RUN=1"
	set "NEXT_ARG=%~2"
	if defined NEXT_ARG (
		if not "!NEXT_ARG:~0,2!"=="--" (
			set "RUN_FILE=!NEXT_ARG!"
			shift
		)
	)
	shift
	goto :parse_args
)
if /i "%~1"=="--build-dir" (
	if "%~2"=="" goto :missing_value
	set "BUILD_DIR=%~2"
	shift
	shift
	goto :parse_args
)
if /i "%~1"=="--source-dir" (
	if "%~2"=="" goto :missing_value
	set "SOURCE_DIR=%~2"
	shift
	shift
	goto :parse_args
)
if /i "%~1"=="--generator" (
	if "%~2"=="" goto :missing_value
	set "GENERATOR=%~2"
	shift
	shift
	goto :parse_args
)
if /i "%~1"=="--target" (
	if "%~2"=="" goto :missing_value
	set "TARGET=%~2"
	shift
	shift
	goto :parse_args
)
if /i "%~1"=="--jobs" (
	if "%~2"=="" goto :missing_value
	set "JOBS=%~2"
	shift
	shift
	goto :parse_args
)
if /i "%~1"=="--cmake-arg" (
	if "%~2"=="" goto :missing_value
	if defined EXTRA_CMAKE_ARGS (
		set "EXTRA_CMAKE_ARGS=!EXTRA_CMAKE_ARGS! %~2"
	) else (
		set "EXTRA_CMAKE_ARGS=%~2"
	)
	shift
	shift
	goto :parse_args
)

echo Unknown option: %~1
goto :help

:missing_value
echo Missing value for %~1
exit /b 1

:main
if "%BUILD_DIR:~-1%"=="\" set "BUILD_DIR=%BUILD_DIR:~0,-1%"
if "%SOURCE_DIR:~-1%"=="\" set "SOURCE_DIR=%SOURCE_DIR:~0,-1%"

if /i "%BUILD_DIR%"=="%SOURCE_DIR%" (
	echo Refusing to use the source directory as the build directory.
	exit /b 1
)

rem --- clean mode: remove the build output and stop --------------------------
if /i "%MODE%"=="clean" (
	if exist "%BUILD_DIR%" (
		echo [ZL] Cleaning "%BUILD_DIR%"...
		rmdir /s /q "%BUILD_DIR%"
		if exist "%BUILD_DIR%" (
			echo [ZL] Failed to remove "%BUILD_DIR%".
			exit /b 1
		)
	) else (
		echo [ZL] Nothing to clean: "%BUILD_DIR%" does not exist.
	)
	echo [ZL] Clean complete.
	exit /b 0
)

rem Detect a running compiler executable before Ninja/CMake attempts to replace it.
rem This prevents a confusing MinGW "Permission denied" linker failure on Windows.
if exist "%BUILD_DIR%\zl_language.exe" (
    tasklist /FI "IMAGENAME eq zl_language.exe" /NH 2>nul | findstr /I /C:"zl_language.exe" >nul
    if not errorlevel 1 (
        echo.
        echo Cannot build: "%BUILD_DIR%\zl_language.exe" is currently running.
        echo Close the running ZL program and run the build again.
        echo.
        exit /b 1
    )
)

if %DO_CLEAN%==1 (
	if exist "%BUILD_DIR%" (
		echo [ZL] Cleaning "%BUILD_DIR%"...
		rmdir /s /q "%BUILD_DIR%"
	)
)

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

rem --- pick the aggregate target for the selected mode ------------------------
rem --target overrides the mode, so single-target builds still work.
if defined TARGET (
	set "BUILD_TARGET=%TARGET%"
	set "MODE_LABEL=target %TARGET%"
) else (
	if /i "%MODE%"=="test" (
		set "BUILD_TARGET=zl-tests"
		set "MODE_LABEL=test"
	) else if /i "%MODE%"=="full" (
		set "BUILD_TARGET=zl-full"
		set "MODE_LABEL=full"
	) else (
		set "BUILD_TARGET=zl-core"
		set "MODE_LABEL=core"
	)
)

if %DO_CONFIGURE%==1 (
	echo [ZL] Configuring !MODE_LABEL! build ^(%CONFIG%^) in "%BUILD_DIR%"...
	if defined GENERATOR (
		cmake -S "%SOURCE_DIR%" -B "%BUILD_DIR%" -G "%GENERATOR%" -DCMAKE_BUILD_TYPE=%CONFIG% %EXTRA_CMAKE_ARGS%
	) else (
		cmake -S "%SOURCE_DIR%" -B "%BUILD_DIR%" -DCMAKE_BUILD_TYPE=%CONFIG% %EXTRA_CMAKE_ARGS%
	)
	if errorlevel 1 exit /b !errorlevel!
)

if %DO_BUILD%==1 (
	if /i "!MODE_LABEL!"=="core" (
		echo [ZL] Building the core toolchain ^(compiler, runtime, VM, required tools^)...
		echo [ZL] Tests, benchmarks and examples are excluded - use "build.bat test" or "build.bat full".
	) else if /i "!MODE_LABEL!"=="test" (
		echo [ZL] Building the core toolchain and all test executables...
	) else if /i "!MODE_LABEL!"=="full" (
		echo [ZL] Building the complete repository ^(core + tests + benchmarks + examples^)...
	) else (
		echo [ZL] Building !MODE_LABEL!...
	)
	echo [ZL] cmake --build "%BUILD_DIR%" --config %CONFIG% --parallel %JOBS% --target !BUILD_TARGET!
	cmake --build "%BUILD_DIR%" --config %CONFIG% --parallel %JOBS% --target !BUILD_TARGET!
	if errorlevel 1 (
		echo [ZL] Build FAILED ^(see the compiler/CMake output above^).
		exit /b !errorlevel!
	)
	if /i "!MODE_LABEL!"=="core" echo [ZL] Core build complete.
	if /i "!MODE_LABEL!"=="test" echo [ZL] Test build complete. Run the suite with: ctest --test-dir "%BUILD_DIR%"
	if /i "!MODE_LABEL!"=="full" echo [ZL] Full build complete.
)

if %DO_CTEST%==1 (
	echo [ZL] Running ctest...
	ctest --test-dir "%BUILD_DIR%" --output-on-failure -C %CONFIG%
	if errorlevel 1 exit /b !errorlevel!
)

if %DO_RUN%==1 (
	set "EXE=%BUILD_DIR%\zl_language.exe"
	if not exist "!EXE!" (
		echo Could not find "!EXE!".
		exit /b 1
	)
	echo [ZL] Running "!EXE!" "!RUN_FILE!"...
	"!EXE!" "!RUN_FILE!"
	exit /b !errorlevel!
)

echo [ZL] Done.
exit /b 0

:help
echo.
echo Usage: scripts\build.bat [mode] [options]
echo.
echo Modes (the default is "core"):
echo   ^(none^) / core    Build only the ZL toolchain: zl_language, zlpkg, zl-bind
echo                    and every compiler/runtime source they need.
echo                    CMake target: zl-core
echo   test             Build the core toolchain plus every C++ test executable.
echo                    CMake target: zl-tests   ^(run them with ctest^)
echo   full             Build the complete repository on purpose: core, tests,
echo                    benchmark and example prerequisites.
echo                    CMake target: zl-full
echo   clean            Delete the build directory.
echo   help             Show this help.
echo.
echo The default build never pulls in tests, stress tests, benchmarks, examples
echo or auxiliary targets - they exist, but only behind "test" and "full".
echo.
echo Options:
echo   --debug              Configure and build Debug instead of Release
echo   --release            Configure and build Release
echo   --clean              Delete the build directory before building
echo   --ctest              Build the tests and then run ctest
echo   --reconfigure        Force the configure step
echo   --no-configure       Skip the configure step
echo   --build-only         Build without configuring
echo   --configure-only     Configure without building
echo   --build-dir PATH     Use a different build directory
echo   --source-dir PATH    Use a different source directory
echo   --generator NAME     Pass an explicit CMake generator
echo   --target NAME        Build one specific target ^(overrides the mode^)
echo   --jobs N             Parallel build jobs ^(default: 4^)
echo   --cmake-arg VALUE    Append an extra cmake configure argument
echo   --run [FILE]         Run build\zl_language.exe after building
echo   -h, --help           Show this help
echo.
echo Examples:
echo   scripts\build.bat
echo   scripts\build.bat test
echo   scripts\build.bat test --ctest
echo   scripts\build.bat full --jobs 8
echo   scripts\build.bat clean
echo   scripts\build.bat --debug --target zl_language
echo   scripts\build.bat --run examples\Hello.zl
exit /b 0
