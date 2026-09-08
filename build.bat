@echo off
setlocal
set "CMAKE_EXE=C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
set "BUILD_DIR=.build"
set "RELEASE_DIR=build\lightency-release"

if not exist "%CMAKE_EXE%" set "CMAKE_EXE=cmake"

"%CMAKE_EXE%" -S . -B "%BUILD_DIR%" -A x64 || exit /b 1
"%CMAKE_EXE%" --build "%BUILD_DIR%" --config Release || exit /b 1

if not exist "%RELEASE_DIR%" mkdir "%RELEASE_DIR%"
if exist "%RELEASE_DIR%\ExplorerTAP.dll" del /Q "%RELEASE_DIR%\ExplorerTAP.dll"
copy /Y "%BUILD_DIR%\Release\lightency.exe" "%RELEASE_DIR%\lightency.exe" >nul || exit /b 1
copy /Y "%BUILD_DIR%\Release\lightency_hook.dll" "%RELEASE_DIR%\lightency_hook.dll" >nul || exit /b 1
copy /Y "LICENSE" "%RELEASE_DIR%\LICENSE.txt" >nul || exit /b 1
copy /Y "THIRD_PARTY_NOTICES.md" "%RELEASE_DIR%\THIRD_PARTY_NOTICES.txt" >nul || exit /b 1

powershell -NoProfile -Command "$s=Get-ChildItem 'C:\Program Files (x86)\Windows Kits' -Filter symsrv.dll -Recurse -ErrorAction SilentlyContinue | Where-Object FullName -match '\\x64\\' | Sort-Object LastWriteTime -Descending | Select-Object -First 1; if(-not $s){exit 1}; Copy-Item -LiteralPath $s.FullName -Destination '%RELEASE_DIR%\symsrv.dll' -Force; Copy-Item -LiteralPath (Join-Path $s.DirectoryName 'dbghelp.dll') -Destination '%RELEASE_DIR%\dbghelp.dll' -Force; Copy-Item -LiteralPath (Join-Path $s.DirectoryName 'dbgcore.dll') -Destination '%RELEASE_DIR%\dbgcore.dll' -Force" || exit /b 1

echo Lightency portable release: %RELEASE_DIR%
