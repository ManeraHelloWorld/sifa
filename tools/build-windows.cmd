@echo off
setlocal EnableExtensions DisableDelayedExpansion

if "%~1"=="" goto :start
if /i "%~1"=="--help" goto :help
echo ERROR: Unknown argument "%~1".
if defined GITHUB_ACTIONS echo ::error::Sifa helper received an unsupported argument.
echo Usage: tools\build-windows.cmd
exit /b 2

:start
pushd "%~dp0.."
if errorlevel 1 (
    echo ERROR: Could not open the Sifa source directory.
    if defined GITHUB_ACTIONS echo ::error::Sifa source directory could not be opened.
    exit /b 1
)
set "SIFA_ROOT=%CD%"
set "SIFA_BUILD=%SIFA_ROOT%\build-msvc"
set "SIFA_VSWHERE="
set "SIFA_VS="
set "SIFA_CMAKE="

if not exist "%SIFA_ROOT%\CMakeLists.txt" goto :missing_sources
if not exist "%SIFA_ROOT%\src\main.cpp" goto :missing_sources
if not exist "%SIFA_ROOT%\web\app.js" goto :missing_sources

if not exist "%SIFA_BUILD%\" mkdir "%SIFA_BUILD%"
if not exist "%SIFA_BUILD%\" goto :build_directory_failed

if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" set "SIFA_VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not defined SIFA_VSWHERE if exist "%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe" set "SIFA_VSWHERE=%ProgramFiles%\Microsoft Visual Studio\Installer\vswhere.exe"
if not defined SIFA_VSWHERE goto :missing_vs

rem Invoke vswhere directly: FOR /F command substitution would reparse its quoted path.
"%SIFA_VSWHERE%" -latest -products * -version "[17.0,18.0)" -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath > "%SIFA_BUILD%\vs-installation.txt"
if errorlevel 1 goto :vs_query_failed
for /f "usebackq delims=" %%I in ("%SIFA_BUILD%\vs-installation.txt") do set "SIFA_VS=%%I"
if not defined SIFA_VS goto :missing_vs

if exist "%SIFA_VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" set "SIFA_CMAKE=%SIFA_VS%\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
if defined SIFA_CMAKE goto :tools_ready
where cmake.exe > "%SIFA_BUILD%\cmake-locations.txt" 2>nul
if errorlevel 1 goto :missing_cmake
for /f "usebackq delims=" %%I in ("%SIFA_BUILD%\cmake-locations.txt") do if not defined SIFA_CMAKE set "SIFA_CMAKE=%%I"
if not defined SIFA_CMAKE goto :missing_cmake

:tools_ready

echo Sifa: local Windows x64 build using Microsoft Visual Studio 2022.
echo Source: "%SIFA_ROOT%"
echo Build:  "%SIFA_BUILD%"
echo Visual Studio: "%SIFA_VS%"
echo CMake: "%SIFA_CMAKE%"
echo This helper compiles the application and tests without starting them.
echo.

"%SIFA_CMAKE%" -S "%SIFA_ROOT%" -B "%SIFA_BUILD%" -G "Visual Studio 17 2022" -A x64 "-DCMAKE_GENERATOR_INSTANCE:PATH=%SIFA_VS%" -DSIFA_BUILD_TESTS=ON -DBUILD_TESTING=ON
if errorlevel 1 goto :configure_failed

"%SIFA_CMAKE%" --build "%SIFA_BUILD%" --config Release --parallel 2
if errorlevel 1 goto :build_failed
if not exist "%SIFA_BUILD%\Release\Sifa.exe" goto :missing_output

echo.
echo BUILD COMPLETE: "%SIFA_BUILD%\Release\Sifa.exe"
echo The application and its tests have NOT been started by this helper.
echo A successful build does not establish that antivirus considers it safe.
echo.
echo NEXT: Follow the Defender verification instructions in README.md.
echo Scan the entire build-msvc directory before starting Sifa or its tests.
echo If Defender reports a threat or the scan fails, stop and keep the report.
echo Do not disable protection or add an exclusion to run this build.
echo.
echo This preview does not run Android or Standoff 2 yet.
popd
exit /b 0

:missing_sources
if defined GITHUB_ACTIONS echo ::error::Sifa source files are missing from this checkout.
echo ERROR: The complete Sifa source tree was not found.
echo Extract or clone the complete repository, then run this helper again.
goto :failed

:build_directory_failed
if defined GITHUB_ACTIONS echo ::error::The Sifa build directory could not be created.
echo ERROR: Could not create "%SIFA_BUILD%".
goto :failed

:vs_query_failed
if defined GITHUB_ACTIONS echo ::error::vswhere failed while querying installed Visual Studio instances.
echo ERROR: vswhere failed while querying Visual Studio.
echo See the preceding command output and "%SIFA_BUILD%\vs-installation.txt".
goto :failed

:missing_vs
if defined GITHUB_ACTIONS echo ::error::Visual Studio 2022 with x64 C++ tools was not found by vswhere.
echo ERROR: Visual Studio 2022 with the C++ toolchain was not found.
echo Install or modify Visual Studio 2022 Build Tools using the Microsoft installer.
echo Select Desktop development with C++, including:
echo   MSVC v143 x64/x86 build tools
echo   Windows 10 or Windows 11 SDK
echo   C++ CMake tools for Windows
echo Official download: https://visualstudio.microsoft.com/visual-cpp-build-tools/
echo Then run this helper again from a normal terminal.
goto :failed

:missing_cmake
if defined GITHUB_ACTIONS echo ::error::CMake was not found in the selected Visual Studio installation or PATH.
echo ERROR: CMake was not found in Visual Studio or PATH.
echo Add C++ CMake tools for Windows through Visual Studio Installer,
echo or install CMake 3.21 or newer and make cmake.exe available in PATH.
goto :failed

:configure_failed
if defined GITHUB_ACTIONS echo ::error::Sifa CMake configuration failed. Inspect the preceding configure output.
echo ERROR: CMake configuration failed. Read the error output above.
echo Check that the Windows SDK and CMake 3.21 or newer are installed.
echo Existing build files were left in place for diagnosis.
goto :failed

:build_failed
if defined GITHUB_ACTIONS echo ::error::Sifa compilation failed. Inspect the preceding compiler output.
echo ERROR: Compilation failed. Read the compiler output above.
echo No Sifa executable or tests were started by this helper.
goto :failed

:missing_output
if defined GITHUB_ACTIONS echo ::error::The build did not produce build-msvc/Release/Sifa.exe.
echo ERROR: The build command succeeded but Release\Sifa.exe was not found.
goto :failed

:failed
popd
exit /b 1

:help
echo Usage: tools\build-windows.cmd
echo Builds Sifa and its tests locally with Visual Studio 2022 for Windows x64.
echo Requires MSVC v143, Windows SDK and CMake 3.21 or newer.
echo Output: build-msvc\Release\Sifa.exe
echo Does not download tools, request elevation, change Defender settings,
echo run Sifa, or execute the compiled tests. Follow README.md to scan first.
exit /b 0
