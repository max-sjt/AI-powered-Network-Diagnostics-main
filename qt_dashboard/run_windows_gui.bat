@echo off
setlocal

cd /d "%~dp0"

set "VS_DEV_CMD=E:\Software\visual 2019\Common7\Tools\VsDevCmd.bat"
set "QT_QMAKE=E:\Software\Qt\6.7.3\msvc2019_64\bin\qmake.exe"
set "QT_DEPLOY=E:\Software\Qt\6.7.3\msvc2019_64\bin\windeployqt.exe"
set "BUILD_DIR=%cd%\build_qt6"

if not exist "%VS_DEV_CMD%" (
    echo Missing Visual Studio developer command script:
    echo %VS_DEV_CMD%
    exit /b 1
)

if not exist "%QT_QMAKE%" (
    echo Missing Qt qmake:
    echo %QT_QMAKE%
    exit /b 1
)

if not exist "%BUILD_DIR%" (
    mkdir "%BUILD_DIR%"
)

cd /d "%BUILD_DIR%"

call "%VS_DEV_CMD%" -arch=x64 -host_arch=x64
if errorlevel 1 goto :fail

"%QT_QMAKE%" ..\WeakNetDashboard.pro
if errorlevel 1 goto :fail

nmake
if errorlevel 1 goto :fail

if exist "%QT_DEPLOY%" (
    "%QT_DEPLOY%" --release --no-translations --no-system-d3d-compiler --no-opengl-sw "%BUILD_DIR%\bin\WeakNetDashboard.exe"
)

if exist "%BUILD_DIR%\bin\WeakNetDashboard.exe" (
    start "" "%BUILD_DIR%\bin\WeakNetDashboard.exe"
    goto :eof
)

echo Build finished, but WeakNetDashboard.exe was not found.
exit /b 1

:fail
echo Build failed.
exit /b 1
