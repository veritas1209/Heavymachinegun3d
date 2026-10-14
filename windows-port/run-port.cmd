@echo off
setlocal
set "PORT_EXE=%~dp0build\bin\heavy_gunner_port.exe"
if not exist "%PORT_EXE%" (
  echo The Windows port has not been built yet.
  echo Run: powershell -ExecutionPolicy Bypass -File "%~dp0build.ps1"
  exit /b 1
)
start "Heavy Gunner 3D Windows Port" "%PORT_EXE%"
