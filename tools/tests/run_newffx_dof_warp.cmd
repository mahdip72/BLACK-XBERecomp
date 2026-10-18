@echo off
setlocal
set "ROOT=%~dp0..\.."
set "BUILD=%ROOT%\out\build\fidelityfx-dof"
if not "%~1"=="" set "BUILD=%~1"
if not exist "%BUILD%\CMakeCache.txt" (
    echo FidelityFX build is not configured: "%BUILD%" 1>&2
    echo Configure it through the project's pinned FidelityFX SDK build first. 1>&2
    exit /b 1
)
cmake --build "%BUILD%" --config Release --target newffx_dof_warp --parallel 2
if errorlevel 1 exit /b 1
set "EXE=%BUILD%\newffx_dof_warp.exe"
if not exist "%EXE%" set "EXE=%BUILD%\Release\newffx_dof_warp.exe"
if not exist "%EXE%" (
    echo Built WARP harness executable not found under "%BUILD%" 1>&2
    exit /b 1
)
pushd "%ROOT%"
"%EXE%"
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%
