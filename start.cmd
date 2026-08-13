@echo off
pushd "%~dp0"

:: Path to MSVC environment setup script
set "VSC_PATH=C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvarsall.bat"
:: Destination for vcpkg installation
set "VCPKG_DEST=%USERPROFILE%\vcpkg"
:: Global binary cache to avoid recompiling dependencies for every build folder
set "VCPKG_CACHE=%USERPROFILE%\.vcpkg\archives"

:: Check path for 'vcvarsall.bat'
if not exist "%VSC_PATH%" (
    echo [ERROR] MSVC environment script ^(vcvarsall.bat^) not found!
    echo Please check your Visual Studio 2022 installation path.
    popd & pause & exit /b
)

:: Check path for 'vcpkg.exe'
if not exist "%VCPKG_DEST%\vcpkg.exe" (
    echo [INFO] vcpkg not found. Starting installation in %VCPKG_DEST%...
    
    :: Get vcpkg from git
    if not exist "%VCPKG_DEST%\" (
        echo [INFO] Cloning vcpkg repository...
        git clone https://github.com/microsoft/vcpkg.git "%VCPKG_DEST%"
        if %errorlevel% neq 0 (
            echo [ERROR] Failed to clone vcpkg repository. Check your internet connection.
            popd & pause & exit /b
        )
    )
    
    echo [INFO] Running bootstrap-vcpkg...
    :: Disable metrics/telemetry during bootstrap
    call "%VCPKG_DEST%\bootstrap-vcpkg.bat" "-disableMetrics"
    
    :: Set persistent environment variables for future sessions
    echo [INFO] Setting up persistent environment variables...
    setx VCPKG_DISABLE_METRICS "1"
    setx VCPKG_ROOT "%VCPKG_DEST%"
)

:: Init MSVC x64
echo [INFO] Initializing MSVC x64 environment...
call "%VSC_PATH%" x64 >nul

:: Set env variables for 'this' session
set "VCPKG_ROOT=%VCPKG_DEST%"
set "VCPKG_DISABLE_METRICS=1"

:: Ensure the binary cache directory exists
if not exist "%VCPKG_CACHE%" mkdir "%VCPKG_CACHE%"
set "VCPKG_DEFAULT_BINARY_CACHE=%VCPKG_CACHE%"

:: Print info:
echo [INFO] VCPKG_ROOT is set to: %VCPKG_ROOT%
echo [INFO] Vcpkg metrics are disabled.
echo [INFO] Binary caching is enabled at: %VCPKG_CACHE%

::
echo [INFO] Searching for available editor...
set "EDITOR_CMD="

:: Check for VSCodium
where codium >nul 2>&1
if %errorlevel% equ 0 (
    set "EDITOR_CMD=codium"
    goto :LAUNCH
)

:: Check for VS Code
where code >nul 2>&1
if %errorlevel% equ 0 (
    set "EDITOR_CMD=code"
    goto :LAUNCH
)

:: Check for full Visual Studio (devenv)
where devenv >nul 2>&1
if %errorlevel% equ 0 (
    echo [INFO] Visual Studio IDE detected. Opening project...
    start "" devenv .
    goto :EXIT_SCRIPT
)

:: Start up ...
:LAUNCH
if defined EDITOR_CMD (
    echo [INFO] Launching %EDITOR_CMD%...
    call %EDITOR_CMD% .
) else (
    echo [WARNING] No professional editor found ^(Codium/VS Code^)!
    echo [INFO] Opening project folder in Explorer...
    explorer .
)

:: Or exit ...
:EXIT_SCRIPT
echo [INFO] Setup complete. IDE launched.
popd
exit