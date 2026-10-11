@echo off
rem Build a 32-bit DXVK d3d9.dll (v3.1.1 + cursor-lock patch) with MSVC.
rem   source\  working copy of tools_and_docs_by_claude\gitignored\dxvk (patched)
rem   out\     resulting d3d9.dll
rem Build files go to temp\dxvk_build_x86 (out of tree). Pass "clean" to rebuild.
setlocal
set "HERE=%~dp0"
set "ROOT=%HERE%..\.."
set "SRC=%HERE%source"
set "BUILD=%ROOT%\temp\dxvk_build_x86"
set "OUT=%HERE%out"
set "TEMP=%ROOT%\temp\msvc_tmp"
set "TMP=%TEMP%"
if not exist "%TEMP%" mkdir "%TEMP%"

if /i "%~1"=="clean" if exist "%BUILD%" rmdir /s /q "%BUILD%"

call "C:\Program Files\Microsoft Visual Studio\18\Community\VC\Auxiliary\Build\vcvarsall.bat" x86 >nul || exit /b 1
set "PATH=%ROOT%\tools_and_docs_by_claude\venv\Scripts;C:\VulkanSDK\1.4.363.0\Bin;%PATH%"

if not exist "%BUILD%\build.ninja" (
  meson setup "%BUILD%" "%SRC%" --buildtype=release --backend=ninja ^
    -Denable_d3d8=false -Denable_d3d10=false -Denable_d3d11=false -Denable_dxgi=false ^
    -Db_vscrt=static_from_buildtype || exit /b 1
)
meson compile -C "%BUILD%" || exit /b 1

if not exist "%OUT%" mkdir "%OUT%"
copy /y "%BUILD%\src\d3d9\d3d9.dll" "%OUT%\d3d9.dll" >nul || exit /b 1
echo Built "%OUT%\d3d9.dll"
