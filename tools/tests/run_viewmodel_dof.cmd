@echo off
setlocal
set "ROOT=%~dp0..\.."
set "OUT=%ROOT%\out\tests\dof-tests"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" (
    echo Visual Studio installer not found: "%VSWHERE%" 1>&2
    exit /b 1
)
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VS=%%i"
if not defined VS (
    echo Visual Studio Build Tools not found 1>&2
    exit /b 1
)
call "%VS%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
if not exist "%OUT%" mkdir "%OUT%"
if errorlevel 1 exit /b 1
pushd "%ROOT%"
cl /nologo /std:c++20 /EHsc /W4 /I src /Fo:"%OUT%\viewmodel_dof_warp.obj" /Fe:"%OUT%\viewmodel_dof_warp.exe" tools\tests\viewmodel_dof_warp.cpp /link d3d11.lib d3dcompiler.lib
if errorlevel 1 (
    popd
    exit /b 1
)
"%OUT%\viewmodel_dof_warp.exe"
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%
