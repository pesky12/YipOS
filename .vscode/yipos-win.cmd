@echo off
setlocal EnableExtensions EnableDelayedExpansion

if "%~1"=="" goto :usage

set "ACTION=%~1"
set "BUILD_TYPE=%~2"
if not defined BUILD_TYPE set "BUILD_TYPE=Debug"
set "OPENVR=%~3"
if not defined OPENVR set "OPENVR=OFF"

for %%I in ("%~dp0..") do set "WORKSPACE_ROOT=%%~fI"
set "SOURCE_DIR=%WORKSPACE_ROOT%\yip_os"
set "BUILD_DIR=%SOURCE_DIR%\build_vscode_%BUILD_TYPE%"

call :find_vs || exit /b 1
if not defined VSCMD_VER (
    call "!VS_VCVARS!" x64
    if errorlevel 1 exit /b 1
)

call :find_vulkan
call :detect_prefixes

if /I "%ACTION%"=="configure" (
    call :configure
    exit /b !ERRORLEVEL!
)

if /I "%ACTION%"=="build" (
    call :build
    exit /b !ERRORLEVEL!
)

if /I "%ACTION%"=="run" (
    call :run
    exit /b !ERRORLEVEL!
)

if /I "%ACTION%"=="clean" (
    call :clean
    exit /b !ERRORLEVEL!
)

echo Unknown action "%ACTION%"
goto :usage

:find_vs
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo VSWhere not found at "%VSWHERE%"
    exit /b 1
)

set "VS_INSTALL="
for /f "usebackq delims=" %%I in (`"%VSWHERE%" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VS_INSTALL=%%I"
if not defined VS_INSTALL (
    echo Visual Studio C++ Build Tools were not found.
    exit /b 1
)

set "VS_VCVARS=%VS_INSTALL%\VC\Auxiliary\Build\vcvarsall.bat"
if not exist "%VS_VCVARS%" (
    echo vcvarsall.bat not found at "%VS_VCVARS%"
    exit /b 1
)
exit /b 0

:find_vulkan
if defined VULKAN_SDK exit /b 0
if not exist "C:\VulkanSDK" exit /b 0
for /f "delims=" %%D in ('dir /b /ad /o-n "C:\VulkanSDK"') do (
    set "VULKAN_SDK=C:\VulkanSDK\%%D"
    goto :eof
)
exit /b 0

:detect_prefixes
set "VCPKG_CMAKE="
if defined VCPKG_ROOT if exist "%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake" (
    set VCPKG_CMAKE=-DCMAKE_TOOLCHAIN_FILE="%VCPKG_ROOT%\scripts\buildsystems\vcpkg.cmake"
    echo vcpkg found at %VCPKG_ROOT%
)

set "CT2_PREFIX_FOUND="
if not defined CT2_PREFIX if exist "C:\ct2_install\lib\cmake\ctranslate2" set "CT2_PREFIX=C:\ct2_install"
if not defined CT2_PREFIX if exist "C:\ct2_install_cpu\lib\cmake\ctranslate2" set "CT2_PREFIX=C:\ct2_install_cpu"
if defined CT2_PREFIX (
    if exist "%CT2_PREFIX%\lib\cmake\ctranslate2" (
        set "CT2_PREFIX_FOUND=%CT2_PREFIX%"
        if exist "%CT2_PREFIX%\bin\cublas64_12.dll" (
            echo CTranslate2 found at %CT2_PREFIX% [CUDA]
        ) else (
            echo CTranslate2 found at %CT2_PREFIX% [CPU-only]
        )
    ) else (
        echo CTranslate2 not found at %CT2_PREFIX% - translation will be disabled
    )
) else (
    echo CTranslate2 not found - translation will be disabled
)

set "MECAB_PREFIX_FOUND="
if not defined MECAB_PREFIX set "MECAB_PREFIX=C:\mecab_install"
if exist "%MECAB_PREFIX%\lib\libmecab.lib" (
    set "MECAB_PREFIX_FOUND=%MECAB_PREFIX%"
    echo MeCab found at %MECAB_PREFIX%
) else (
    echo MeCab not found at %MECAB_PREFIX% - Japanese kanji will display as '?'
)

set "PREFIX_PATH="
if defined CT2_PREFIX_FOUND set "PREFIX_PATH=%CT2_PREFIX_FOUND%"
if defined MECAB_PREFIX_FOUND (
    if defined PREFIX_PATH (
        set "PREFIX_PATH=%PREFIX_PATH%;%MECAB_PREFIX_FOUND%"
    ) else (
        set "PREFIX_PATH=%MECAB_PREFIX_FOUND%"
    )
)

set "PREFIX_CMAKE="
if defined PREFIX_PATH set PREFIX_CMAKE=-DCMAKE_PREFIX_PATH="%PREFIX_PATH%"
exit /b 0

:configure
echo.
echo === Configuring YipOS %BUILD_TYPE% ^(OpenVR %OPENVR%^) ===
cmake -S "%SOURCE_DIR%" -B "%BUILD_DIR%" -G Ninja -DCMAKE_BUILD_TYPE=%BUILD_TYPE% -DCMAKE_EXPORT_COMPILE_COMMANDS=ON -DGGML_CCACHE=OFF -DYIPOS_HAS_OPENVR=%OPENVR% %VCPKG_CMAKE% %PREFIX_CMAKE%
exit /b %ERRORLEVEL%

:build
call :configure
if errorlevel 1 exit /b 1

echo.
echo === Building YipOS %BUILD_TYPE% ===
cmake --build "%BUILD_DIR%"
if errorlevel 1 exit /b 1

call :copy_optional_runtime
if errorlevel 1 exit /b 1

echo.
echo Built: %BUILD_DIR%\yip_os.exe
exit /b 0

:run
if not exist "%BUILD_DIR%\yip_os.exe" (
    call :build
    if errorlevel 1 exit /b 1
)

pushd "%BUILD_DIR%"
"%BUILD_DIR%\yip_os.exe"
set "RUN_EXIT=%ERRORLEVEL%"
popd
exit /b %RUN_EXIT%

:clean
if exist "%BUILD_DIR%" (
    echo Removing %BUILD_DIR%
    rmdir /s /q "%BUILD_DIR%"
)
exit /b 0

:copy_optional_runtime
if defined CT2_PREFIX_FOUND (
    copy /Y "%CT2_PREFIX_FOUND%\bin\ctranslate2.dll" "%BUILD_DIR%\" >nul 2>nul
    copy /Y "%CT2_PREFIX_FOUND%\bin\sentencepiece.dll" "%BUILD_DIR%\" >nul 2>nul
    copy /Y "%CT2_PREFIX_FOUND%\bin\openblas.dll" "%BUILD_DIR%\" >nul 2>nul
    copy /Y "%CT2_PREFIX_FOUND%\bin\cublas64_12.dll" "%BUILD_DIR%\" >nul 2>nul
    copy /Y "%CT2_PREFIX_FOUND%\bin\cublasLt64_12.dll" "%BUILD_DIR%\" >nul 2>nul
    copy /Y "%CT2_PREFIX_FOUND%\bin\cudart64_12.dll" "%BUILD_DIR%\" >nul 2>nul
)

if defined MECAB_PREFIX_FOUND (
    copy /Y "%MECAB_PREFIX_FOUND%\bin\libmecab.dll" "%BUILD_DIR%\" >nul 2>nul
    if exist "%MECAB_PREFIX_FOUND%\dic\ipadic" (
        if not exist "%BUILD_DIR%\mecab-dic\ipadic" mkdir "%BUILD_DIR%\mecab-dic\ipadic"
        xcopy /Y /Q "%MECAB_PREFIX_FOUND%\dic\ipadic\*.*" "%BUILD_DIR%\mecab-dic\ipadic\" >nul
        > "%BUILD_DIR%\mecabrc" echo dicdir = mecab-dic\ipadic
    )
)
exit /b 0

:usage
echo Usage: %~nx0 ^<configure^|build^|run^|clean^> [Debug^|Release^|RelWithDebInfo] [ON^|OFF]
echo Example: %~nx0 build Debug OFF
exit /b 1
