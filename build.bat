@echo off
setlocal
set "CMAKE_EXE=cmake"
where "%CMAKE_EXE%" >nul 2>nul
if %errorlevel% neq 0 (
    if exist "%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
        set "CMAKE_EXE=%ProgramFiles%\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    ) else if exist "%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
        set "CMAKE_EXE=%ProgramFiles%\Microsoft Visual Studio\2022\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    ) else if exist "%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" (
        set "CMAKE_EXE=%ProgramFiles(x86)%\Microsoft Visual Studio\2019\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe"
    )
)

set "VERSION=1.1.1"
for /f "tokens=3" %%v in ('findstr /i "project.*VERSION" CMakeLists.txt') do set "VERSION=%%v"

set "BUILD_DIR=.build"
set "RELEASE_DIR=build\lightency-release"
set "ZIP_NAME=Lightency-%VERSION%-win-x64.zip"
set "ZIP_PATH=build\%ZIP_NAME%"

del /f /q "%BUILD_DIR%\Release\*.old.*" 2>nul
if exist "%BUILD_DIR%\Release\lightency_hook.dll" move /y "%BUILD_DIR%\Release\lightency_hook.dll" "%BUILD_DIR%\Release\lightency_hook.dll.old.%RANDOM%" >nul 2>nul
if exist "%BUILD_DIR%\Release\lightency.exe" move /y "%BUILD_DIR%\Release\lightency.exe" "%BUILD_DIR%\Release\lightency.exe.old.%RANDOM%" >nul 2>nul


"%CMAKE_EXE%" -S . -B "%BUILD_DIR%" -A x64 || exit /b 1
"%CMAKE_EXE%" --build "%BUILD_DIR%" --config Release || exit /b 1

if not exist "%RELEASE_DIR%" mkdir "%RELEASE_DIR%"
del /f /q "%RELEASE_DIR%\*.old*" 2>nul
del /f /q "%RELEASE_DIR%\*.tmp" 2>nul
if exist "%RELEASE_DIR%\lightency.exe" move /y "%RELEASE_DIR%\lightency.exe" "%RELEASE_DIR%\lightency.exe.old.%RANDOM%" >nul 2>nul
if exist "%RELEASE_DIR%\lightency_hook.dll" move /y "%RELEASE_DIR%\lightency_hook.dll" "%RELEASE_DIR%\lightency_hook.dll.old.%RANDOM%" >nul 2>nul

copy /Y "%BUILD_DIR%\Release\lightency.exe" "%RELEASE_DIR%\lightency.exe" >nul || exit /b 1
copy /Y "%BUILD_DIR%\Release\lightency_hook.dll" "%RELEASE_DIR%\lightency_hook.dll" >nul || exit /b 1
copy /Y "LICENSE" "%RELEASE_DIR%\LICENSE.txt" >nul || exit /b 1

if exist "%ZIP_PATH%" del /f /q "%ZIP_PATH%"
powershell -NoProfile -Command "Get-ChildItem -Path '%RELEASE_DIR%' -Exclude '*.old*' | Compress-Archive -DestinationPath '%ZIP_PATH%' -Force" || exit /b 1

echo.
echo Lightency portable release built in: %RELEASE_DIR%
echo ZIP archive created: %ZIP_PATH%
powershell -NoProfile -Command "Start-Process '%~dp0%RELEASE_DIR%\lightency.exe' -WorkingDirectory '%~dp0%RELEASE_DIR%'"

