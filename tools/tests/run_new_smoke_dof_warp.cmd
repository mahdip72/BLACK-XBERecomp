@echo off
setlocal
set "ROOT=%~dp0..\.."
set "BUILD=%ROOT%\out\build\fidelityfx-dof"
if not "%~1"=="" set "BUILD=%~1"
if not exist "%BUILD%\CMakeCache.txt" (
    echo FidelityFX build is not configured: "%BUILD%" 1>&2
    exit /b 1
)
cmake --build "%BUILD%" --config Release --target new_smoke_dof_warp --parallel 2
if errorlevel 1 exit /b 1
set "EXE=%BUILD%\new_smoke_dof_warp.exe"
if not exist "%EXE%" set "EXE=%BUILD%\Release\new_smoke_dof_warp.exe"
if not exist "%EXE%" (
    echo Built smoke WARP executable not found under "%BUILD%" 1>&2
    exit /b 1
)
pushd "%ROOT%"
"%EXE%"
set "RESULT=%ERRORLEVEL%"
popd
exit /b %RESULT%
