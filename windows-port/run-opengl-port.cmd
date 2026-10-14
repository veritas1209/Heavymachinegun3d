@echo off
setlocal
set "PORT_EXE=%~dp0build\bin\heavy_gunner_gl.exe"
if not exist "%PORT_EXE%" (
  echo The OpenGL Windows port has not been built yet.
  echo Run: powershell -ExecutionPolicy Bypass -File "%~dp0build.ps1"
  exit /b 1
)
start "Heavy Gunner 3D OpenGL Port" "%PORT_EXE%" --game
