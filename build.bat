@echo off
setlocal

set CMAKE="C:\Program Files\CMake\bin\cmake.exe"
set BUILD_DIR=build_ninja

if not exist %BUILD_DIR% (
    echo Configuring...
    %CMAKE% -S . -B %BUILD_DIR% -G Ninja
    if errorlevel 1 (
        echo CMake configure failed.
        exit /b 1
    )
)

echo Building...
%CMAKE% --build %BUILD_DIR%
if errorlevel 1 (
    echo Build failed.
    exit /b 1
)

echo Build succeeded.
endlocal
