@echo off
setlocal
set "ROOT=%~dp0..\.."
set "OUT=%ROOT%\out\tests\dof-tests"
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "%VSWHERE%" exit /b 1
for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -products * -property installationPath`) do set "VS=%%i"
if not defined VS exit /b 1
call "%VS%\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 exit /b 1
if not exist "%OUT%" mkdir "%OUT%"
if errorlevel 1 exit /b 1
pushd "%ROOT%"
cl /nologo /std:c++20 /EHsc /W4 /I tools\tests\stubs /I src /c /Fo:"%OUT%\depth_of_field_hook.obj" src\game\depth_of_field.cpp
if errorlevel 1 (
    popd
    exit /b 1
)
cl /nologo /std:c++20 /EHsc /W4 /I tools\tests\stubs /I src /c /Fo:"%OUT%\dof_hook.obj" tools\tests\dof_hook.cpp
if errorlevel 1 (
    popd
    exit /b 1
)
link /nologo /out:"%OUT%\dof_hook.exe" "%OUT%\depth_of_field_hook.obj" "%OUT%\dof_hook.obj"
if errorlevel 1 (
    popd
    exit /b 1
)
"%OUT%\dof_hook.exe"
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%
