@echo off
setlocal EnableExtensions EnableDelayedExpansion
pushd "%~dp0"

echo [INFO] Cleaning CMake / Visual Studio / runtime leftovers in %CD%

rem --- salvage runtime DBG logs + message.db* into doc\evidence\run-<UTC> ---
call :preserve_evidence

call :rmdir_if out
call :rmdir_if build
call :rmdir_if bin
call :rmdir_if cmake-build-debug
call :rmdir_if cmake-build-release
call :rmdir_if .vs
call :rmdir_if .cache
call :rmdir_if CMakeFiles
call :rmdir_if CMakeScripts
call :rmdir_if Testing
call :rmdir_if .cmake
call :rmdir_if ipch
call :rmdir_if vcpkg_installed
call :rmdir_if ci

for /d %%D in (build-*) do (
    echo [INFO] Removing %%D
    rmdir /s /q "%%D" 2>nul
)

call :rmfile_if CMakeCache.txt
call :rmfile_if cmake_install.cmake
call :rmfile_if CTestTestfile.cmake
call :rmfile_if CTestCustom.cmake
call :rmfile_if DartConfiguration.tcl
call :rmfile_if compile_commands.json
call :rmfile_if CMakeUserPresets.json
call :rmfile_if CMakeSettings.json
call :rmfile_if .ninja_log
call :rmfile_if .ninja_deps
call :rmfile_if vcpkg-manifest-install.log
rem message.db* moved by preserve_evidence (not deleted)
call :rmfile_if ci-summary.json
call :rmfile_if cppcheck_report.txt
call :rmfile_if lizard_result.txt
call :rmfile_if test_output.txt

del /f /q *.gcda *.gcno *.gcov 2>nul

rem Nested in-source cmake junk under apps/ src/ tests/ (paths baked into cmake_install.cmake)
echo [INFO] Removing nested CMakeFiles / cmake_install under apps src tests
for %%R in (apps src tests) do (
    if exist "%%R\" (
        for /d /r "%%R" %%D in (CMakeFiles) do (
            if exist "%%D\" (
                echo [INFO] Removing %%D\
                rmdir /s /q "%%D" 2>nul
            )
        )
        for /d /r "%%R" %%D in (CMakeScripts) do (
            if exist "%%D\" (
                echo [INFO] Removing %%D\
                rmdir /s /q "%%D" 2>nul
            )
        )
        for /d /r "%%R" %%D in (Testing) do (
            if exist "%%D\" (
                echo [INFO] Removing %%D\
                rmdir /s /q "%%D" 2>nul
            )
        )
        for /r "%%R" %%F in (cmake_install.cmake CTestTestfile.cmake CTestCustom.cmake CMakeCache.txt Makefile build.ninja rules.ninja .ninja_log .ninja_deps compile_commands.json) do (
            if exist "%%F" (
                echo [INFO] Removing %%F
                del /f /q "%%F" 2>nul
            )
        )
        for /r "%%R" %%F in (*.o *.obj *.a *.lib *.pdb *.ilk) do (
            if exist "%%F" del /f /q "%%F" 2>nul
        )
    )
)

echo [INFO] Clean done. Source, CMakeLists, presets, vcpkg.json, and doc\evidence\ were left alone.
echo [INFO] Salvaged logs/DB (if any) are under doc\evidence\run-^<UTC^>\.
echo [INFO] Close Visual Studio first if .vs or out could not be removed.
echo [INFO] Rebuild: start.cmd then cmake -B build -S . ^&^& cmake --build build
echo [INFO] Test:    ctest --test-dir build --output-on-failure
popd
endlocal
exit /b 0

:preserve_evidence
if not exist "doc\evidence\" mkdir "doc\evidence" >nul 2>&1
for /f %%I in ('powershell -NoProfile -Command "Get-Date -Format yyyyMMdd-HHmmss"') do set "EVSTAMP=%%I"
set "EVDEST=doc\evidence\run-%EVSTAMP%"
set "MOVED=0"

for %%S in (bin\logs build\logs build\bin\logs out\logs log) do (
    if exist "%%S\" (
        dir /a /b "%%S" 2>nul | findstr /r "." >nul && (
            if not exist "%EVDEST%\" mkdir "%EVDEST%" >nul 2>&1
            echo [INFO] Preserving logs from %%S\ -^> %EVDEST%\
            xcopy /e /i /y /q "%%S\*" "%EVDEST%\" >nul 2>&1
            set "MOVED=1"
        )
    )
)

if exist "logs\" (
    for /f "delims=" %%F in ('dir /a /b "logs" 2^>nul') do (
        if not exist "%EVDEST%\" mkdir "%EVDEST%" >nul 2>&1
        echo [INFO] Moving logs\%%F -^> %EVDEST%\
        if exist "logs\%%F\" (
            xcopy /e /i /y /q "logs\%%F\*" "%EVDEST%\" >nul 2>&1
            rmdir /s /q "logs\%%F" 2>nul
        ) else (
            move /y "logs\%%F" "%EVDEST%\" >nul 2>&1
        )
        set "MOVED=1"
    )
    rmdir /s /q "logs" 2>nul
)

for %%F in (message.db message.db-wal message.db-shm message.db-journal) do (
    if exist "%%F" (
        if not exist "%EVDEST%\" mkdir "%EVDEST%" >nul 2>&1
        echo [INFO] Moving %%F -^> %EVDEST%\
        move /y "%%F" "%EVDEST%\" >nul 2>&1
        set "MOVED=1"
    )
    if exist "build\%%F" (
        if not exist "%EVDEST%\" mkdir "%EVDEST%" >nul 2>&1
        echo [INFO] Moving build\%%F -^> %EVDEST%\
        move /y "build\%%F" "%EVDEST%\" >nul 2>&1
        set "MOVED=1"
    )
    if exist "build\bin\%%F" (
        if not exist "%EVDEST%\" mkdir "%EVDEST%" >nul 2>&1
        echo [INFO] Moving build\bin\%%F -^> %EVDEST%\
        move /y "build\bin\%%F" "%EVDEST%\" >nul 2>&1
        set "MOVED=1"
    )
    if exist "bin\%%F" (
        if not exist "%EVDEST%\" mkdir "%EVDEST%" >nul 2>&1
        echo [INFO] Moving bin\%%F -^> %EVDEST%\
        move /y "bin\%%F" "%EVDEST%\" >nul 2>&1
        set "MOVED=1"
    )
)

if "%MOVED%"=="1" echo [INFO] Evidence (logs/DB) saved under %EVDEST%\
exit /b 0

:rmdir_if
if exist "%~1\" (
    echo [INFO] Removing %~1\
    rmdir /s /q "%~1" 2>nul
    if exist "%~1\" echo [WARN] Could not remove %~1\ — file in use?
)
exit /b 0

:rmfile_if
if exist "%~1" (
    echo [INFO] Removing %~1
    del /f /q "%~1" 2>nul
)
exit /b 0
