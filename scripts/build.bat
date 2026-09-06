@echo off
setlocal EnableExtensions EnableDelayedExpansion

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
set "RUN_FILE=%SOURCE_DIR%\examples\Hello.zl"
set "EXTRA_CMAKE_ARGS="

:parse_args
if "%~1"=="" goto :main

if /i "%~1"=="-h" goto :help
if /i "%~1"=="--help" goto :help
if /i "%~1"=="help" goto :help

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
if /i "%BUILD_DIR%"=="%SOURCE_DIR%" (
	echo Refusing to use the source directory as the build directory.
	exit /b 1
)

if %DO_CLEAN%==1 (
	if exist "%BUILD_DIR%" (
		echo Cleaning "%BUILD_DIR%"...
		rmdir /s /q "%BUILD_DIR%"
	)
)

if not exist "%BUILD_DIR%" mkdir "%BUILD_DIR%"

if %DO_CONFIGURE%==1 (
	echo Configuring %CONFIG% build in "%BUILD_DIR%"...
	if defined GENERATOR (
		cmake -S "%SOURCE_DIR%" -B "%BUILD_DIR%" -G "%GENERATOR%" -DCMAKE_BUILD_TYPE=%CONFIG% %EXTRA_CMAKE_ARGS%
	) else (
		cmake -S "%SOURCE_DIR%" -B "%BUILD_DIR%" -DCMAKE_BUILD_TYPE=%CONFIG% %EXTRA_CMAKE_ARGS%
	)
	if errorlevel 1 exit /b %errorlevel%
)

if %DO_BUILD%==1 (
	if defined TARGET (
		echo Building target "%TARGET%" with %JOBS% jobs...
		cmake --build "%BUILD_DIR%" --config %CONFIG% --parallel %JOBS% --target "%TARGET%"
	) else (
		echo Building with %JOBS% jobs...
		cmake --build "%BUILD_DIR%" --config %CONFIG% --parallel %JOBS%
	)
	if errorlevel 1 exit /b %errorlevel%
)

if %DO_RUN%==1 (
	set "EXE=%BUILD_DIR%\zl_language.exe"
	if not exist "!EXE!" (
		echo Could not find "!EXE!".
		exit /b 1
	)
	echo Running "!EXE!" "!RUN_FILE!"...
	"!EXE!" "!RUN_FILE!"
	exit /b %errorlevel%
)

echo Done.
exit /b 0

:help
echo.
echo Usage: scripts/build.bat [options]
echo.
echo Default: configure Release into .\build and build with 4 jobs.
echo.
echo Options:
echo   --debug              Configure and build Debug instead of Release
echo   --release            Configure and build Release
echo   --clean              Delete the build directory before building
echo   --reconfigure        Force the configure step
echo   --no-configure       Skip the configure step
echo   --build-only         Build without configuring
echo   --configure-only     Configure without building
echo   --build-dir PATH     Use a different build directory
echo   --source-dir PATH    Use a different source directory
echo   --generator NAME     Pass an explicit CMake generator
echo   --target NAME        Build only one target
echo   --jobs N             Parallel build jobs (default: 4)
echo   --cmake-arg VALUE    Append an extra cmake configure argument
echo   --run [FILE]         Run build\zl_language.exe after building
echo   -h, --help           Show this help
echo.
echo Examples:
echo   scripts/build.bat
echo   scripts/build.bat --debug --clean
echo   scripts/build.bat --target zl_language --jobs 8
echo   scripts/build.bat --run examples\Hello.zl
exit /b 0
