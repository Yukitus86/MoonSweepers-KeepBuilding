@echo off
setlocal
cd /d "%~dp0"

rem --- find Visual Studio (any edition / insiders) with C++ tools ---
set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"
set "VSDIR="
if exist "%VSWHERE%" (
  for /f "usebackq delims=" %%i in (`"%VSWHERE%" -latest -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath`) do set "VSDIR=%%i"
)
if not defined VSDIR (
  for /d %%d in ("%ProgramFiles%\Microsoft Visual Studio\*\*") do if exist "%%d\VC\Auxiliary\Build\vcvars64.bat" set "VSDIR=%%d"
)
if not defined VSDIR (
  echo Visual Studio with C++ tools not found.
  exit /b 1
)
call "%VSDIR%\VC\Auxiliary\Build\vcvars64.bat" >nul || exit /b 1

if not exist build mkdir build
if not exist dist mkdir dist

ml64 /nologo /c /Fo build\dwmapi_thunks.obj src\dwmapi_thunks.asm || exit /b 1
rc /nologo /fo build\KeepBuilding.res src\KeepBuilding.rc || exit /b 1
cl /nologo /O2 /MT /W4 /LD /Fo:build\ src\KeepBuilding.c build\dwmapi_thunks.obj build\KeepBuilding.res ^
   /link /DLL /DEF:src\dwmapi.def /OUT:build\dwmapi.dll /IMPLIB:build\dwmapi.lib kernel32.lib || exit /b 1

copy /y build\dwmapi.dll dist\dwmapi.dll >nul
copy /y KeepBuilding.ini dist\KeepBuilding.ini >nul
if exist README.md copy /y README.md dist\README.md >nul
echo.
echo Build OK: dist\dwmapi.dll
